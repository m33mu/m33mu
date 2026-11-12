/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include "mcxa153/mcxa153_mmio.h"
#include "mcxa153/mcxa153_romapi.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/gpio.h"

/* SYSCON and MRCC0 share one 4 KB block (MCXA153_COMMON.h) */
#define SYSCON_BASE 0x40091000u
#define SCG_BASE    0x4008F000u
#define SPC_BASE    0x40090000u
#define FMU_BASE    0x40095000u
#define VBAT_BASE   0x40093000u
#define CMC_BASE    0x4008B000u
#define PORT0_BASE  0x400BC000u
#define GPIO0_BASE  0x40102000u
#define BLOCK_SIZE  0x1000u
#define PORT_COUNT  4u
#define GPIO_COUNT  4u

#define MRCC_GLB_RST0     0x000u
#define MRCC_GLB_RST0_SET 0x004u
#define MRCC_GLB_RST0_CLR 0x008u
#define MRCC_GLB_RST1     0x010u
#define MRCC_GLB_RST1_SET 0x014u
#define MRCC_GLB_RST1_CLR 0x018u
#define MRCC_GLB_CC0      0x040u
#define MRCC_GLB_CC0_SET  0x044u
#define MRCC_GLB_CC0_CLR  0x048u
#define MRCC_GLB_CC1      0x050u
#define MRCC_GLB_CC1_SET  0x054u
#define MRCC_GLB_CC1_CLR  0x058u
#define SYSCON_AHBCLKDIV  0x380u
#define SYSCON_CLKUNLOCK  0x3FCu

#define SCG_CSR     0x010u
#define SCG_RCCR    0x014u
#define SCG_SOSCCSR 0x100u
#define SCG_SIRCCSR 0x200u
#define SCG_FIRCCSR 0x300u
#define SCG_FIRCCFG 0x308u
#define SCG_ROSCCSR 0x400u
#define SCG_SCS_MASK 0x07000000u
#define SCG_CSR_EN   (1u << 0)
#define SCG_CSR_VLD  (1u << 24)

#define SPC_SC         0x010u
#define SPC_SRAMCTL    0x040u
#define SPC_ACTIVE_CFG 0x100u
#define SPC_SRAMCTL_REQ (1u << 30)
#define SPC_SRAMCTL_ACK (1u << 31)
/* BGMODE=1, CORELDO_VDD_LVL=normal, CORELDO_VDD_DS=normal */
#define SPC_ACTIVE_CFG_RESET 0x00100109u

#define FMU_FSTAT 0x000u
#define FMU_FSTAT_CCIF (1u << 7)

struct regblock {
    mm_u32 base;
    mm_u32 regs[BLOCK_SIZE / 4];
};

static struct regblock syscon;
static struct regblock scg;
static struct regblock spc;
static struct regblock fmu;
static struct regblock vbat;
static struct regblock cmc;
static struct regblock ports[PORT_COUNT];
static struct regblock gpios[GPIO_COUNT];

static mm_bool block_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct regblock *b = (struct regblock *)opaque;
    if (b == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > BLOCK_SIZE) return MM_FALSE;
    *value_out = 0;
    memcpy(value_out, (mm_u8 *)b->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool block_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct regblock *b = (struct regblock *)opaque;
    if (b == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > BLOCK_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)b->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool syscon_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    switch (offset) {
    case MRCC_GLB_RST0_SET:
    case MRCC_GLB_RST0_CLR:
    case MRCC_GLB_RST1_SET:
    case MRCC_GLB_RST1_CLR:
    case MRCC_GLB_CC0_SET:
    case MRCC_GLB_CC0_CLR:
    case MRCC_GLB_CC1_SET:
    case MRCC_GLB_CC1_CLR:
        if (value_out == 0 || size_bytes != 4u) return MM_FALSE;
        *value_out = 0u;
        return MM_TRUE;
    default:
        break;
    }
    return block_read(opaque, offset, size_bytes, value_out);
}

static mm_bool syscon_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct regblock *b = (struct regblock *)opaque;
    mm_u32 target;
    mm_bool set;
    if (b == 0) return MM_FALSE;
    switch (offset) {
    case MRCC_GLB_RST0_SET: target = MRCC_GLB_RST0; set = MM_TRUE; break;
    case MRCC_GLB_RST0_CLR: target = MRCC_GLB_RST0; set = MM_FALSE; break;
    case MRCC_GLB_RST1_SET: target = MRCC_GLB_RST1; set = MM_TRUE; break;
    case MRCC_GLB_RST1_CLR: target = MRCC_GLB_RST1; set = MM_FALSE; break;
    case MRCC_GLB_CC0_SET: target = MRCC_GLB_CC0; set = MM_TRUE; break;
    case MRCC_GLB_CC0_CLR: target = MRCC_GLB_CC0; set = MM_FALSE; break;
    case MRCC_GLB_CC1_SET: target = MRCC_GLB_CC1; set = MM_TRUE; break;
    case MRCC_GLB_CC1_CLR: target = MRCC_GLB_CC1; set = MM_FALSE; break;
    default:
        return block_write(opaque, offset, size_bytes, value);
    }
    if (size_bytes != 4u) return MM_FALSE;
    if (set) {
        b->regs[target / 4] |= value;
    } else {
        b->regs[target / 4] &= ~value;
    }
    return MM_TRUE;
}

mm_bool mm_mcxa153_mrcc_enabled(mm_u32 bit)
{
    if (bit >= 32u) return MM_FALSE;
    if ((syscon.regs[MRCC_GLB_CC0 / 4] & (1u << bit)) == 0u) return MM_FALSE;
    return ((syscon.regs[MRCC_GLB_RST0 / 4] & (1u << bit)) != 0u) ? MM_TRUE : MM_FALSE;
}

/* Oscillator status registers: VLD follows the enable bit, RCCR.SCS is
 * reflected in CSR.SCS at once (fsl_clock.c polls both). */
static mm_bool scg_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct regblock *b = (struct regblock *)opaque;
    if (b == 0 || size_bytes != 4u) return MM_FALSE;
    if ((offset + size_bytes) > BLOCK_SIZE) return MM_FALSE;
    if (offset == SCG_CSR) return MM_TRUE;
    switch (offset) {
    case SCG_RCCR:
        b->regs[SCG_RCCR / 4] = value;
        b->regs[SCG_CSR / 4] = (b->regs[SCG_CSR / 4] & ~SCG_SCS_MASK) | (value & SCG_SCS_MASK);
        return MM_TRUE;
    case SCG_SOSCCSR:
    case SCG_SIRCCSR:
    case SCG_FIRCCSR:
    case SCG_ROSCCSR:
        value &= ~SCG_CSR_VLD;
        if ((value & SCG_CSR_EN) != 0u) {
            value |= SCG_CSR_VLD;
        }
        b->regs[offset / 4] = value;
        return MM_TRUE;
    default:
        break;
    }
    b->regs[offset / 4] = value;
    return MM_TRUE;
}

static mm_bool spc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    if (offset == SPC_SC && size_bytes == 4u && value_out != 0) {
        *value_out = 0u;
        return MM_TRUE;
    }
    return block_read(opaque, offset, size_bytes, value_out);
}

/* SRAMCTL.REQ is acknowledged at once; the SDK spins on ACK */
static mm_bool spc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct regblock *b = (struct regblock *)opaque;
    if (b == 0) return MM_FALSE;
    if (offset == SPC_SRAMCTL && size_bytes == 4u) {
        value &= ~SPC_SRAMCTL_ACK;
        if ((value & SPC_SRAMCTL_REQ) != 0u) {
            value |= SPC_SRAMCTL_ACK;
        }
        b->regs[SPC_SRAMCTL / 4] = value;
        return MM_TRUE;
    }
    return block_write(opaque, offset, size_bytes, value);
}

static mm_bool mcxa153_gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)GPIO_COUNT) {
        return MM_FALSE;
    }
    if (name_out != 0 && name_len > 0u) {
        snprintf(name_out, name_len, "P%d", bank);
    }
    if (pins_out != 0) {
        *pins_out = 32;
    }
    return MM_TRUE;
}

static mm_bool mcxa153_rcc_clock_list_line(void *opaque, int line, char *out, size_t out_len)
{
    (void)opaque;
    if (out == 0 || out_len == 0u || line != 0) {
        return MM_FALSE;
    }
    snprintf(out, out_len, "SCG: SCS=%lu FIRC=%s SIRC=%s",
             (unsigned long)((scg.regs[SCG_CSR / 4] & SCG_SCS_MASK) >> 24),
             ((scg.regs[SCG_FIRCCSR / 4] & SCG_CSR_VLD) != 0u) ? "on" : "off",
             ((scg.regs[SCG_SIRCCSR / 4] & SCG_CSR_VLD) != 0u) ? "on" : "off");
    return MM_TRUE;
}

static mm_bool register_block(struct mmio_bus *bus, struct regblock *b, mm_u32 base,
                              mmio_read_fn read, mmio_write_fn write)
{
    struct mmio_region reg;
    memset(&reg, 0, sizeof(reg));
    b->base = base;
    reg.base = base;
    reg.size = BLOCK_SIZE;
    reg.opaque = b;
    reg.read = (read != 0) ? read : block_read;
    reg.write = (write != 0) ? write : block_write;
    return mmio_bus_register_region(bus, &reg);
}

void mm_mcxa153_mmio_reset(void)
{
    mm_u32 i;
    memset(&syscon, 0, sizeof(syscon));
    memset(&scg, 0, sizeof(scg));
    memset(&spc, 0, sizeof(spc));
    memset(&fmu, 0, sizeof(fmu));
    memset(&vbat, 0, sizeof(vbat));
    memset(&cmc, 0, sizeof(cmc));
    for (i = 0; i < PORT_COUNT; ++i) {
        memset(ports[i].regs, 0, sizeof(ports[i].regs));
    }
    for (i = 0; i < GPIO_COUNT; ++i) {
        memset(gpios[i].regs, 0, sizeof(gpios[i].regs));
    }
    /* Out of reset the core runs from FRO12M (SIRC); FIRC is off */
    syscon.regs[SYSCON_CLKUNLOCK / 4] = 1u;
    scg.regs[SCG_CSR / 4] = 0x02000000u;
    scg.regs[SCG_RCCR / 4] = 0x02000000u;
    scg.regs[SCG_SIRCCSR / 4] = SCG_CSR_EN | SCG_CSR_VLD;
    scg.regs[SCG_FIRCCFG / 4] = 0x0000000Au;
    spc.regs[SPC_ACTIVE_CFG / 4] = SPC_ACTIVE_CFG_RESET;
    fmu.regs[FMU_FSTAT / 4] = FMU_FSTAT_CCIF;
    mm_gpio_set_bank_info_reader(mcxa153_gpio_bank_info, 0);
    mm_rcc_set_clock_list_reader(mcxa153_rcc_clock_list_line, 0);
    mm_mcxa153_romapi_reset();
}

mm_bool mm_mcxa153_register_mmio(struct mmio_bus *bus)
{
    mm_u32 i;
    if (bus == 0) return MM_FALSE;
    if (!register_block(bus, &syscon, SYSCON_BASE, syscon_read, syscon_write)) return MM_FALSE;
    if (!register_block(bus, &scg, SCG_BASE, 0, scg_write)) return MM_FALSE;
    if (!register_block(bus, &spc, SPC_BASE, spc_read, spc_write)) return MM_FALSE;
    if (!register_block(bus, &fmu, FMU_BASE, 0, 0)) return MM_FALSE;
    if (!register_block(bus, &vbat, VBAT_BASE, 0, 0)) return MM_FALSE;
    if (!register_block(bus, &cmc, CMC_BASE, 0, 0)) return MM_FALSE;
    for (i = 0; i < PORT_COUNT; ++i) {
        if (!register_block(bus, &ports[i], PORT0_BASE + i * BLOCK_SIZE, 0, 0)) return MM_FALSE;
    }
    for (i = 0; i < GPIO_COUNT; ++i) {
        if (!register_block(bus, &gpios[i], GPIO0_BASE + i * BLOCK_SIZE, 0, 0)) return MM_FALSE;
    }
    if (!mm_mcxa153_romapi_register_mmio(bus)) return MM_FALSE;
    return MM_TRUE;
}

/* Flash is programmed through the ROM API only: a CPU store into the flash
 * window faults, as on silicon. */
void mm_mcxa153_flash_bind(struct mm_memmap *map,
                           mm_u8 *flash,
                           mm_u32 flash_size,
                           const struct mm_flash_persist *persist,
                           mm_u32 flags)
{
    (void)map;
    (void)flash;
    (void)flash_size;
    (void)flags;
    mm_mcxa153_romapi_set_persist(persist);
}

mm_u64 mm_mcxa153_cpu_hz(void)
{
    return 96000000ull;
}
