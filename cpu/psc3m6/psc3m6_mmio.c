/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Infineon PSOC Control C3M6: PERI slave gating, PERI_PCLK dividers, HSIOM
 * pin muxing with the per-port non-secure mask, GPIO ports and the six SCB
 * serial blocks (UART and SPI master).  Register layouts follow the PDL IP
 * headers (cyip_peri_v2.h, cyip_peri_pclk_v2.h, cyip_hsiom.h, cyip_gpio.h,
 * cyip_scb_v4_2.h).  Flash is only programmed through the boot ROM API
 * (psc3m6_romapi.c); CPU stores into the flash aliases bus-fault.
 */

#include <string.h>
#include <stdio.h>
#include "psc3m6/psc3m6_mmio.h"
#include "psc3m6/psc3m6_romapi.h"
#include "psc3m6/cpu_config.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/target_hal.h"
#include "m33mu/spi_bus.h"

#define S_ALIAS 0x10000000u

/* PERI: group slave control at 0x4000 + 0x40 * group. */
#define PERI_BASE_NS        0x42000000u
#define PERI_SIZE           0x10000u
#define PERI_GR_BASE        0x4000u
#define PERI_GR_STRIDE      0x40u
#define PERI_GR_COUNT       6u
#define PERI_GR_SL_CTL      0x10u
#define PERI_GR_SL_CTL2     0x14u
#define PERI_GR_SL_CTL3     0x18u
/* PERI group and slave of each modelled block (CY_MMIO_*_GROUP_NR /
 * _SLAVE_NR in the kit's psc3m6_config.h); a disabled slave bus-faults. */
#define PERI_HSIOM_GR       1u
#define PERI_HSIOM_SL       0u
#define PERI_GPIO_GR        1u
#define PERI_GPIO_SL        1u

/* PERI_PCLK: one 0x2000 block per group, 7 groups on this part. */
#define PCLK_BASE_NS        0x42040000u
#define PCLK_SIZE           0x20000u
#define PCLK_GR_STRIDE      0x2000u
#define PCLK_GR_COUNT       7u
#define PCLK_DIV_CMD        0x000u
#define PCLK_CLOCK_CTL      0xC00u
#define PCLK_DIV_8_CTL      0x1000u
#define PCLK_DIV_16_CTL     0x1400u
#define PCLK_DIV_16_5_CTL   0x1800u
#define PCLK_DIV_24_5_CTL   0x1C00u
#define PCLK_DIV_CMD_ENABLE  (1u << 31)
#define PCLK_DIV_CMD_DISABLE (1u << 30)
#define PCLK_DIV_EN          (1u << 0)
#define PCLK_TYPE_8          0u
#define PCLK_TYPE_16         1u
#define PCLK_TYPE_16_5       2u
#define PCLK_TYPE_24_5       3u
/* Reset values: every destination disconnected (type 3, divider 255). */
#define PCLK_CLOCK_CTL_RESET 0x000003FFu
#define PCLK_DIV_CMD_RESET   0x03FF03FFu

/* HSIOM: PRT[p] at 0x10 * p, SECURE_PRT[p] at 0x1000 + 0x10 * p. */
#define HSIOM_BASE_NS       0x42400000u
#define HSIOM_SIZE          0x3000u
#define HSIOM_PORT_COUNT    14u
#define HSIOM_PRT_STRIDE    0x10u
#define HSIOM_SECURE_BASE   0x1000u

/* GPIO: PRT[p] at 0x80 * p. */
#define GPIO_BASE_NS        0x42410000u
#define GPIO_SIZE           0x9000u
#define GPIO_PRT_STRIDE     0x80u
#define GPIO_OUT            0x00u
#define GPIO_OUT_CLR        0x04u
#define GPIO_OUT_SET        0x08u
#define GPIO_OUT_INV        0x0Cu
#define GPIO_IN             0x10u
#define GPIO_CFG            0x44u

/* SCB (cyip_scb_v4_2.h). */
#define SCB_COUNT           6u
#define SCB_SIZE            0x1000u
#define SCB_CTRL            0x000u
#define SCB_STATUS          0x004u
#define SCB_SPI_CTRL        0x020u
#define SCB_SPI_STATUS      0x024u
#define SCB_UART_CTRL       0x040u
#define SCB_TX_CTRL         0x200u
#define SCB_TX_FIFO_CTRL    0x204u
#define SCB_TX_FIFO_STATUS  0x208u
#define SCB_TX_FIFO_WR      0x240u
#define SCB_RX_CTRL         0x300u
#define SCB_RX_FIFO_CTRL    0x304u
#define SCB_RX_FIFO_STATUS  0x308u
#define SCB_RX_FIFO_RD      0x340u
#define SCB_RX_FIFO_RD_SILENT 0x344u
#define SCB_CTRL_ENABLED    (1u << 31)
#define SCB_CTRL_MODE_Pos   24u
#define SCB_CTRL_MODE_Msk   (3u << SCB_CTRL_MODE_Pos)
#define SCB_MODE_I2C        0u
#define SCB_MODE_SPI        1u
#define SCB_MODE_UART       2u
#define SCB_SPI_CTRL_MASTER (1u << 31)
#define SCB_SPI_CTRL_SSEL_CONTINUOUS (1u << 0)
#define SCB_FIFO_CTRL_CLEAR (1u << 16)
#define SCB_RX_RING         256u

struct peri_state {
    mm_u32 regs[PERI_SIZE / 4];
};

struct pclk_state {
    mm_u32 regs[PCLK_SIZE / 4];
};

struct hsiom_state {
    mm_u32 regs[HSIOM_SIZE / 4];
};

struct gpio_state {
    mm_u32 regs[GPIO_SIZE / 4];
};

struct scb_inst {
    mm_u32 index;
    mm_u32 base;
    mm_u32 pclk_gr;
    mm_u32 pclk_idx;
    mm_u32 peri_gr;
    mm_u32 peri_sl;
    mm_u32 regs[SCB_SIZE / 4];
    struct mm_uart_io io;
    mm_bool io_open;
    char label[8];
    mm_u8 rx_ring[SCB_RX_RING];
    mm_u32 rx_head;
    mm_u32 rx_count;
};

static struct peri_state peri;
static struct pclk_state pclk;
static struct hsiom_state hsiom;
static struct gpio_state gpio;
static struct scb_inst scbs[SCB_COUNT];
static mm_bool scb_active = MM_FALSE;

/* Peripheral clock destination of each SCB.  The PSC3M5 headers put SCBn at
 * group 4 index n; on the C3M6 the console SCB3 is driven through index 6
 * (confirmed by perturbing the divider on a running part), one SCB every
 * two indexes.  SCB5 keeps the header's group 6 index 0. */
static const mm_u32 scb_base_ns[SCB_COUNT] = {
    0x42820000u, 0x42840000u, 0x42850000u, 0x42860000u, 0x42870000u, 0x42C00000u
};
static const mm_u32 scb_pclk_gr[SCB_COUNT] = { 4u, 4u, 4u, 4u, 4u, 6u };
static const mm_u32 scb_pclk_idx[SCB_COUNT] = { 0u, 2u, 4u, 6u, 8u, 0u };
static const mm_u32 scb_peri_gr[SCB_COUNT] = { 3u, 3u, 3u, 3u, 3u, 5u };
static const mm_u32 scb_peri_sl[SCB_COUNT] = { 1u, 2u, 3u, 4u, 5u, 0u };

static mm_u32 read_slice(mm_u32 reg, mm_u32 offset_in_reg, mm_u32 size_bytes)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xFFFFFFFFu : ((1u << (size_bytes * 8u)) - 1u);
    return (reg >> shift) & mask;
}

static mm_u32 apply_write(mm_u32 cur, mm_u32 offset_in_reg, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xFFFFFFFFu : ((1u << (size_bytes * 8u)) - 1u);
    mm_u32 shifted = (value & mask) << shift;
    return (cur & ~(mask << shift)) | shifted;
}

static mm_bool access_ok(mm_u32 offset, mm_u32 size_bytes, mm_u32 limit)
{
    if (size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if ((offset & 3u) + size_bytes > 4u) return MM_FALSE;
    return (offset + size_bytes) <= limit;
}

static mm_bool reg_pair(struct mmio_bus *bus, mm_u32 ns_base, mm_u32 size,
                        void *opaque, mmio_read_fn rd, mmio_write_fn wr)
{
    struct mmio_region reg;
    memset(&reg, 0, sizeof(reg));
    reg.size = size;
    reg.opaque = opaque;
    reg.read = rd;
    reg.write = wr;
    reg.base = ns_base;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = ns_base | S_ALIAS;
    return mmio_bus_register_region(bus, &reg);
}

/* ---- PERI ----------------------------------------------------------- */

static void peri_reset(void)
{
    memset(&peri, 0, sizeof(peri));
    /* Group 0 is enabled out of reset, the others are gated. */
    peri.regs[(PERI_GR_BASE + PERI_GR_SL_CTL) / 4] = 0xFFFFFFFFu;
}

mm_bool mm_psc3m6_peri_slave_enabled(mm_u32 gr, mm_u32 slave)
{
    if (gr >= PERI_GR_COUNT || slave >= 32u) return MM_FALSE;
    return (peri.regs[(PERI_GR_BASE + gr * PERI_GR_STRIDE + PERI_GR_SL_CTL) / 4] &
            (1u << slave)) != 0u;
}

static mm_bool peri_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || !access_ok(offset, size_bytes, PERI_SIZE)) return MM_FALSE;
    *value_out = read_slice(peri.regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool peri_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 reg = offset & ~3u;
    (void)opaque;
    if (!access_ok(offset, size_bytes, PERI_SIZE)) return MM_FALSE;
    if (reg >= PERI_GR_BASE && reg < PERI_GR_BASE + PERI_GR_COUNT * PERI_GR_STRIDE) {
        mm_u32 in_gr = (reg - PERI_GR_BASE) % PERI_GR_STRIDE;
        if (in_gr == PERI_GR_SL_CTL3) {
            return MM_TRUE; /* read-only power state */
        }
    }
    peri.regs[offset / 4] = apply_write(peri.regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

/* ---- PERI_PCLK ------------------------------------------------------ */

static mm_u32 pclk_div_ctl_offset(mm_u32 type)
{
    switch (type) {
    case PCLK_TYPE_8: return PCLK_DIV_8_CTL;
    case PCLK_TYPE_16: return PCLK_DIV_16_CTL;
    case PCLK_TYPE_16_5: return PCLK_DIV_16_5_CTL;
    default: return PCLK_DIV_24_5_CTL;
    }
}

mm_bool mm_psc3m6_pclk_active(mm_u32 gr, mm_u32 idx)
{
    mm_u32 ctl;
    mm_u32 type;
    mm_u32 div;
    mm_u32 off;
    if (gr >= PCLK_GR_COUNT || idx >= 256u) return MM_FALSE;
    ctl = pclk.regs[(gr * PCLK_GR_STRIDE + PCLK_CLOCK_CTL + idx * 4u) / 4];
    type = (ctl >> 8) & 3u;
    div = ctl & 0xFFu;
    if (type == PCLK_TYPE_24_5 && div >= 255u) return MM_FALSE;
    off = gr * PCLK_GR_STRIDE + pclk_div_ctl_offset(type) + div * 4u;
    return (pclk.regs[off / 4] & PCLK_DIV_EN) != 0u;
}

static mm_bool pclk_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || !access_ok(offset, size_bytes, PCLK_SIZE)) return MM_FALSE;
    if (offset / PCLK_GR_STRIDE >= PCLK_GR_COUNT) return MM_FALSE;
    *value_out = read_slice(pclk.regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool pclk_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 gr = offset / PCLK_GR_STRIDE;
    mm_u32 in_gr = (offset % PCLK_GR_STRIDE) & ~3u;
    mm_u32 cur;
    mm_u32 next;
    (void)opaque;
    if (!access_ok(offset, size_bytes, PCLK_SIZE)) return MM_FALSE;
    if (gr >= PCLK_GR_COUNT) return MM_FALSE;
    cur = pclk.regs[offset / 4];
    next = apply_write(cur, offset & 3u, size_bytes, value);
    if (in_gr == PCLK_DIV_CMD) {
        /* ENABLE/DISABLE act at once and read back clear; the divider's EN
         * bit is status, only DIV_CMD changes it. */
        mm_u32 type = (next >> 8) & 3u;
        mm_u32 div = next & 0xFFu;
        mm_u32 off;
        if (type == PCLK_TYPE_24_5 && div >= 255u) {
            return MM_TRUE;
        }
        off = gr * PCLK_GR_STRIDE + pclk_div_ctl_offset(type) + div * 4u;
        if ((next & PCLK_DIV_CMD_ENABLE) != 0u) {
            pclk.regs[off / 4] |= PCLK_DIV_EN;
        } else if ((next & PCLK_DIV_CMD_DISABLE) != 0u) {
            pclk.regs[off / 4] &= ~PCLK_DIV_EN;
        }
        pclk.regs[offset / 4] = next & ~(PCLK_DIV_CMD_ENABLE | PCLK_DIV_CMD_DISABLE);
        return MM_TRUE;
    }
    if (in_gr >= PCLK_DIV_8_CTL) {
        next = (next & ~PCLK_DIV_EN) | (cur & PCLK_DIV_EN);
    }
    pclk.regs[offset / 4] = next;
    return MM_TRUE;
}

/* ---- HSIOM / GPIO ---------------------------------------------------- */

/* SECURE_PRT[p].NONSECURE_MASK splits each port between the aliases: a pin
 * is visible through the Secure alias when its bit is clear and through the
 * Non-secure alias when it is set; the other alias drops writes to it and
 * reads it as zero. */
static mm_u32 port_ns_mask(mm_u32 port)
{
    if (port >= HSIOM_PORT_COUNT) return 0u;
    return hsiom.regs[(HSIOM_SECURE_BASE + port * HSIOM_PRT_STRIDE) / 4] & 0xFFu;
}

/* Bit mask over a 32-bit register of the pins the Secure alias may touch,
 * for fields of `bits` per pin starting at pin `first`. */
static mm_u32 alias_pin_mask(mm_u32 port, mm_bool secure)
{
    mm_u32 ns = port_ns_mask(port);
    return secure ? (~ns & 0xFFu) : ns;
}

static mm_u32 secure_pin_field_mask(mm_u32 port, mm_u32 bits, mm_u32 first, mm_bool secure)
{
    mm_u32 visible = alias_pin_mask(port, secure);
    mm_u32 mask = 0u;
    mm_u32 pin;
    mm_u32 field = (bits == 32u) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    for (pin = 0; pin < 8u && (first + pin) < 8u; ++pin) {
        if (((visible >> (first + pin)) & 1u) != 0u && (pin * bits) < 32u) {
            mask |= field << (pin * bits);
        }
    }
    return mask;
}

static void hsiom_reset(void)
{
    mm_u32 p;
    memset(&hsiom, 0, sizeof(hsiom));
    /* The boot ROM hands every pin over marked non-secure. */
    for (p = 0; p < HSIOM_PORT_COUNT; ++p) {
        hsiom.regs[(HSIOM_SECURE_BASE + p * HSIOM_PRT_STRIDE) / 4] = 0xFFu;
    }
}

static mm_bool hsiom_access(mm_u32 offset, mm_bool secure, mm_u32 *mask_out, mm_bool *masked)
{
    mm_u32 reg = offset & ~3u;
    *masked = MM_FALSE;
    *mask_out = 0xFFFFFFFFu;
    if (reg < HSIOM_PORT_COUNT * HSIOM_PRT_STRIDE) {
        mm_u32 port = reg / HSIOM_PRT_STRIDE;
        mm_u32 in_prt = reg % HSIOM_PRT_STRIDE;
        if (in_prt == 0u || in_prt == 4u) {
            *masked = MM_TRUE;
            *mask_out = secure_pin_field_mask(port, 8u, (in_prt == 0u) ? 0u : 4u, secure);
        }
    }
    return MM_TRUE;
}

static mm_bool hsiom_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_bool secure = (opaque != 0);
    mm_u32 mask;
    mm_bool masked;
    mm_u32 v;
    if (value_out == 0 || !access_ok(offset, size_bytes, HSIOM_SIZE)) return MM_FALSE;
    if (!mm_psc3m6_peri_slave_enabled(PERI_HSIOM_GR, PERI_HSIOM_SL)) return MM_FALSE;
    (void)hsiom_access(offset, secure, &mask, &masked);
    v = hsiom.regs[offset / 4] & mask;
    *value_out = read_slice(v, offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool hsiom_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_bool secure = (opaque != 0);
    mm_u32 mask;
    mm_bool masked;
    mm_u32 cur;
    mm_u32 next;
    mm_u32 reg = offset & ~3u;
    if (!access_ok(offset, size_bytes, HSIOM_SIZE)) return MM_FALSE;
    if (!mm_psc3m6_peri_slave_enabled(PERI_HSIOM_GR, PERI_HSIOM_SL)) return MM_FALSE;
    if (reg >= HSIOM_SECURE_BASE && reg < HSIOM_SECURE_BASE + HSIOM_PORT_COUNT * HSIOM_PRT_STRIDE) {
        if ((reg % HSIOM_PRT_STRIDE) != 0u) return MM_TRUE;
        /* Only the Secure alias may change the non-secure mask. */
        if (!secure) return MM_FALSE;
    }
    (void)hsiom_access(offset, secure, &mask, &masked);
    cur = hsiom.regs[offset / 4];
    next = apply_write(cur, offset & 3u, size_bytes, value);
    hsiom.regs[offset / 4] = (cur & ~mask) | (next & mask);
    return MM_TRUE;
}

static mm_u32 gpio_port_mask(mm_u32 offset, mm_bool secure)
{
    mm_u32 reg = offset & ~3u;
    mm_u32 port = reg / GPIO_PRT_STRIDE;
    mm_u32 in_prt = reg % GPIO_PRT_STRIDE;
    if (port >= HSIOM_PORT_COUNT) return 0xFFFFFFFFu;
    if (in_prt == GPIO_CFG) {
        return secure_pin_field_mask(port, 4u, 0u, secure);
    }
    if (in_prt <= GPIO_IN) {
        return alias_pin_mask(port, secure);
    }
    return 0xFFFFFFFFu;
}

static mm_bool gpio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_bool secure = (opaque != 0);
    mm_u32 reg = offset & ~3u;
    mm_u32 v;
    if (value_out == 0 || !access_ok(offset, size_bytes, GPIO_SIZE)) return MM_FALSE;
    if (!mm_psc3m6_peri_slave_enabled(PERI_GPIO_GR, PERI_GPIO_SL)) return MM_FALSE;
    if (reg < HSIOM_PORT_COUNT * GPIO_PRT_STRIDE && (reg % GPIO_PRT_STRIDE) == GPIO_IN) {
        /* Nothing drives the pins from outside: read back the output latch. */
        v = gpio.regs[(reg - GPIO_IN + GPIO_OUT) / 4];
    } else {
        v = gpio.regs[offset / 4];
    }
    v &= gpio_port_mask(offset, secure);
    *value_out = read_slice(v, offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool gpio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_bool secure = (opaque != 0);
    mm_u32 reg = offset & ~3u;
    mm_u32 mask;
    mm_u32 cur;
    mm_u32 next;
    if (!access_ok(offset, size_bytes, GPIO_SIZE)) return MM_FALSE;
    if (!mm_psc3m6_peri_slave_enabled(PERI_GPIO_GR, PERI_GPIO_SL)) return MM_FALSE;
    mask = gpio_port_mask(offset, secure);
    if (reg < HSIOM_PORT_COUNT * GPIO_PRT_STRIDE) {
        mm_u32 in_prt = reg % GPIO_PRT_STRIDE;
        mm_u32 out_idx = (reg - in_prt + GPIO_OUT) / 4;
        mm_u32 bits = apply_write(0u, offset & 3u, size_bytes, value) & mask & 0xFFu;
        if (in_prt == GPIO_OUT_CLR) {
            gpio.regs[out_idx] &= ~bits;
            return MM_TRUE;
        }
        if (in_prt == GPIO_OUT_SET) {
            gpio.regs[out_idx] |= bits;
            return MM_TRUE;
        }
        if (in_prt == GPIO_OUT_INV) {
            gpio.regs[out_idx] ^= bits;
            return MM_TRUE;
        }
        if (in_prt == GPIO_IN) {
            return MM_TRUE;
        }
    }
    cur = gpio.regs[offset / 4];
    next = apply_write(cur, offset & 3u, size_bytes, value);
    gpio.regs[offset / 4] = (cur & ~mask) | (next & mask);
    return MM_TRUE;
}

/* ---- SCB ------------------------------------------------------------- */

static mm_u32 scb_mode(const struct scb_inst *s)
{
    return (s->regs[SCB_CTRL / 4] & SCB_CTRL_MODE_Msk) >> SCB_CTRL_MODE_Pos;
}

static mm_bool scb_enabled(const struct scb_inst *s)
{
    return (s->regs[SCB_CTRL / 4] & SCB_CTRL_ENABLED) != 0u;
}

static void scb_rx_push(struct scb_inst *s, mm_u8 byte)
{
    if (s->rx_count >= SCB_RX_RING) return;
    s->rx_ring[(s->rx_head + s->rx_count) % SCB_RX_RING] = byte;
    s->rx_count++;
}

static mm_bool scb_rx_pop(struct scb_inst *s, mm_bool peek, mm_u8 *byte_out)
{
    if (s->rx_count == 0u) return MM_FALSE;
    *byte_out = s->rx_ring[s->rx_head];
    if (!peek) {
        s->rx_head = (s->rx_head + 1u) % SCB_RX_RING;
        s->rx_count--;
    }
    return MM_TRUE;
}

static mm_u32 scb_rx_used(const struct scb_inst *s)
{
    mm_u32 used = s->rx_count;
    if (scb_mode(s) == SCB_MODE_UART && s->io_open && mm_uart_io_has_rx(&s->io)) {
        used++;
    }
    return used;
}

static mm_bool scb_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct scb_inst *s = (struct scb_inst *)opaque;
    mm_u32 reg = offset & ~3u;
    mm_u32 v;
    if (s == 0 || value_out == 0 || !access_ok(offset, size_bytes, SCB_SIZE)) return MM_FALSE;
    /* An SCB that is gated or has no running peripheral clock bus-faults. */
    if (!mm_psc3m6_peri_slave_enabled(s->peri_gr, s->peri_sl)) return MM_FALSE;
    if (!mm_psc3m6_pclk_active(s->pclk_gr, s->pclk_idx)) return MM_FALSE;
    switch (reg) {
    case SCB_TX_FIFO_STATUS:
        v = 0u; /* bytes leave the FIFO as they are written */
        break;
    case SCB_RX_FIFO_STATUS:
        v = scb_rx_used(s) & 0x1FFu;
        break;
    case SCB_RX_FIFO_RD:
    case SCB_RX_FIFO_RD_SILENT: {
        mm_bool peek = (reg == SCB_RX_FIFO_RD_SILENT) || mmio_peek_mode();
        mm_u8 byte = 0u;
        v = 0u;
        if (scb_rx_pop(s, peek, &byte)) {
            v = byte;
        } else if (scb_mode(s) == SCB_MODE_UART && s->io_open && mm_uart_io_has_rx(&s->io)) {
            v = peek ? (mm_u32)mm_uart_io_peek(&s->io) : (mm_u32)mm_uart_io_read(&s->io);
        }
        break;
    }
    default:
        v = s->regs[reg / 4];
        break;
    }
    *value_out = read_slice(v, offset & 3u, size_bytes);
    return MM_TRUE;
}

static void scb_tx_byte(struct scb_inst *s, mm_u8 byte)
{
    mm_u32 mode = scb_mode(s);
    if (!scb_enabled(s)) return;
    if (mode == SCB_MODE_UART) {
        if (s->io_open) {
            mm_uart_io_queue_tx(&s->io, byte);
            (void)mm_uart_io_flush(&s->io);
        }
    } else if (mode == SCB_MODE_SPI) {
        mm_u32 spi_ctrl = s->regs[SCB_SPI_CTRL / 4];
        if ((spi_ctrl & SCB_SPI_CTRL_MASTER) != 0u) {
            scb_rx_push(s, mm_spi_bus_xfer((int)s->index, byte));
            /* Without SSEL_CONTINUOUS the select drops between frames. */
            if ((spi_ctrl & SCB_SPI_CTRL_SSEL_CONTINUOUS) == 0u) {
                mm_spi_bus_end((int)s->index);
            }
        }
    }
}

static mm_bool scb_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct scb_inst *s = (struct scb_inst *)opaque;
    mm_u32 reg = offset & ~3u;
    mm_u32 cur;
    mm_u32 next;
    if (s == 0 || !access_ok(offset, size_bytes, SCB_SIZE)) return MM_FALSE;
    if (!mm_psc3m6_peri_slave_enabled(s->peri_gr, s->peri_sl)) return MM_FALSE;
    if (!mm_psc3m6_pclk_active(s->pclk_gr, s->pclk_idx)) return MM_FALSE;
    cur = s->regs[reg / 4];
    next = apply_write(cur, offset & 3u, size_bytes, value);
    switch (reg) {
    case SCB_TX_FIFO_WR:
        scb_tx_byte(s, (mm_u8)(next & 0xFFu));
        return MM_TRUE;
    case SCB_TX_FIFO_STATUS:
    case SCB_RX_FIFO_STATUS:
    case SCB_RX_FIFO_RD:
    case SCB_RX_FIFO_RD_SILENT:
    case SCB_STATUS:
    case SCB_SPI_STATUS:
        return MM_TRUE;
    case SCB_RX_FIFO_CTRL:
        if ((next & SCB_FIFO_CTRL_CLEAR) != 0u) {
            s->rx_head = 0u;
            s->rx_count = 0u;
        }
        break;
    case SCB_CTRL:
        if ((cur & SCB_CTRL_ENABLED) != 0u && (next & SCB_CTRL_ENABLED) == 0u &&
            scb_mode(s) == SCB_MODE_SPI) {
            mm_spi_bus_end((int)s->index);
        }
        break;
    default:
        break;
    }
    s->regs[reg / 4] = next;
    return MM_TRUE;
}

void mm_psc3m6_scb_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    /* The kit console SCB3 opens first so --uart-stdout lands on it. */
    static const mm_u32 order[SCB_COUNT] = { 3u, 0u, 1u, 2u, 4u, 5u };
    mm_u32 k;
    (void)nvic;
    for (k = 0; k < SCB_COUNT; ++k) {
        mm_u32 i = order[k];
        struct scb_inst *s = &scbs[i];
        memset(s, 0, sizeof(*s));
        s->index = i;
        s->base = scb_base_ns[i];
        s->pclk_gr = scb_pclk_gr[i];
        s->pclk_idx = scb_pclk_idx[i];
        s->peri_gr = scb_peri_gr[i];
        s->peri_sl = scb_peri_sl[i];
        mm_uart_io_init(&s->io);
        snprintf(s->label, sizeof(s->label), "SCB%u", (unsigned)i);
        if (!reg_pair(bus, s->base, SCB_SIZE, s, scb_read, scb_write)) {
            continue;
        }
        if (mm_uart_io_open(&s->io, s->base)) {
            s->io_open = MM_TRUE;
            if (mm_tui_is_active()) {
                mm_tui_attach_uart(s->label, s->io.name);
            }
        }
    }
    scb_active = MM_TRUE;
}

void mm_psc3m6_scb_reset(void)
{
    mm_u32 i;
    for (i = 0; i < SCB_COUNT; ++i) {
        if (scbs[i].io_open) {
            mm_uart_io_close(&scbs[i].io);
        }
        memset(&scbs[i], 0, sizeof(scbs[i]));
    }
    scb_active = MM_FALSE;
}

void mm_psc3m6_scb_poll(void)
{
    mm_u32 i;
    if (!scb_active) return;
    for (i = 0; i < SCB_COUNT; ++i) {
        if (scbs[i].io_open) {
            (void)mm_uart_io_poll(&scbs[i].io);
        }
    }
}

/* ---- target hooks ---------------------------------------------------- */

static void pclk_reset(void)
{
    mm_u32 gr;
    mm_u32 i;
    memset(&pclk, 0, sizeof(pclk));
    for (gr = 0; gr < PCLK_GR_COUNT; ++gr) {
        pclk.regs[(gr * PCLK_GR_STRIDE + PCLK_DIV_CMD) / 4] = PCLK_DIV_CMD_RESET;
        for (i = 0; i < 256u; ++i) {
            pclk.regs[(gr * PCLK_GR_STRIDE + PCLK_CLOCK_CTL + i * 4u) / 4] = PCLK_CLOCK_CTL_RESET;
        }
    }
}

void mm_psc3m6_mmio_reset(void)
{
    peri_reset();
    pclk_reset();
    hsiom_reset();
    memset(&gpio, 0, sizeof(gpio));
    mm_psc3m6_romapi_reset();
}

mm_bool mm_psc3m6_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    mm_psc3m6_mmio_reset();
    if (!reg_pair(bus, PERI_BASE_NS, PERI_SIZE, 0, peri_read, peri_write)) return MM_FALSE;
    if (!reg_pair(bus, PCLK_BASE_NS, PCLK_SIZE, 0, pclk_read, pclk_write)) return MM_FALSE;
    /* HSIOM and GPIO tell the aliases apart: the opaque flags the Secure one. */
    memset(&reg, 0, sizeof(reg));
    reg.size = HSIOM_SIZE;
    reg.read = hsiom_read;
    reg.write = hsiom_write;
    reg.base = HSIOM_BASE_NS;
    reg.opaque = 0;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = HSIOM_BASE_NS | S_ALIAS;
    reg.opaque = &hsiom;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.size = GPIO_SIZE;
    reg.read = gpio_read;
    reg.write = gpio_write;
    reg.base = GPIO_BASE_NS;
    reg.opaque = 0;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GPIO_BASE_NS | S_ALIAS;
    reg.opaque = &gpio;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    return mm_psc3m6_romapi_register_mmio(bus);
}

static mm_bool flash_store_cb(void *opaque, enum mm_sec_state sec, mm_u32 addr,
                              mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque; (void)sec; (void)addr; (void)size_bytes; (void)value;
    /* Flash is programmed only through the boot ROM API: a CPU store faults. */
    return MM_FALSE;
}

void mm_psc3m6_flash_bind(struct mm_memmap *map,
                          mm_u8 *flash,
                          mm_u32 flash_size,
                          const struct mm_flash_persist *persist,
                          mm_u32 flags)
{
    (void)flags;
    if (map == 0) return;
    mm_psc3m6_romapi_bind(map, flash, flash_size, persist);
    mm_memmap_set_flash_writer(map, flash_store_cb, 0);
}

mm_u64 mm_psc3m6_cpu_hz(void)
{
    /* IHO: the clock the part wakes up on; wolfBoot leaves the tree alone. */
    return 48000000ull;
}
