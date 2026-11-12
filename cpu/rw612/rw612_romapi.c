/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * RW612 boot ROM model (0x13000000 secure / 0x03000000 non-secure, 256 KB).
 *
 * The ROM API tree sits where the SDK looks for it (fsl_romapi.c
 * get_romapi_addr()): 0x13024100 on A0 silicon, 0x13030000 on A1 and later,
 * selected by SOCCTRL CHIP_INFO[3:0].  Only the tree of the modelled
 * revision is populated.  Its layout follows bootloader_tree_t, and entry [5]
 * points at a flexspi_nor_flash_driver_t (fsl_romapi_flexspi.h) whose
 * functions program, erase and read the FlexSPI NOR model.  Every entry
 * point is a slot in a stub area; calls are trapped by PC from
 * mm_bootapi_handle and never execute ROM code.
 *
 * The synthetic tree the first RW612 model exposed at 0x1301F000
 * (flash_init / flash_get_property / version) is kept for existing users.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rw612/rw612_romapi.h"
#include "rw612/rw612_flexspi.h"
#include "rw612/rw612_mmio.h"
#include "rw612/cpu_config.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"

#define ROM_ALIAS_DELTA (RW612_ROM_BASE_S - RW612_ROM_BASE_NS)

/* Offsets inside the ROM window. */
#define LEGACY_STUB_OFF      0x01000u
#define LEGACY_TREE_OFF      0x1F000u
#define LEGACY_TREE_SIZE     0x100u
#define TREE_A0_OFF          0x24100u
#define TREE_A1_OFF          0x30000u
#define FLEXSPI_DRV_OFF      0x3E000u
#define COPYRIGHT_OFF        0x3E100u
#define STUB_OFF             0x3E200u

#define ROMAPI_PTR(addr)        ((mm_u32)(addr) | 1u)
#define ROMAPI_VERSION(a,maj,min,bug) \
    ((mm_u32)(bug) | ((mm_u32)(min) << 8u) | ((mm_u32)(maj) << 16u) | ((mm_u32)(a) << 24u))

/* Legacy synthetic tree: entry points in the non-secure alias. */
#define LEGACY_STUB_FLASH_INIT          (RW612_ROM_BASE_NS + LEGACY_STUB_OFF + 0x000u)
#define LEGACY_STUB_FLASH_GET_PROPERTY  (RW612_ROM_BASE_NS + LEGACY_STUB_OFF + 0x010u)
#define LEGACY_STUB_VERSION             (RW612_ROM_BASE_NS + LEGACY_STUB_OFF + 0x020u)
#define LEGACY_STUB_SIZE                0x30u

#define RW612_PFLASH_PAGE_SIZE   256u
#define RW612_PFLASH_SECTOR_SIZE 4096u
#define RW612_SYS_FREQ_HZ        64000000u

#define FC_PFLASH_BLOCK_BASE    0x00u
#define FC_PFLASH_TOTAL_SIZE    0x04u
#define FC_PFLASH_BLOCK_COUNT   0x08u
#define FC_PFLASH_PAGE_SIZE     0x0Cu
#define FC_PFLASH_SECTOR_SIZE   0x10u

#define FLASH_PROP_SECTOR_SIZE  0x00u
#define FLASH_PROP_TOTAL_SIZE   0x01u
#define FLASH_PROP_BLOCK_SIZE   0x02u
#define FLASH_PROP_BLOCK_COUNT  0x03u
#define FLASH_PROP_START_ADDR   0x04u
#define FLASH_PROP_PAGE_SIZE    0x30u
#define FLASH_PROP_SYS_FREQ     0x31u

#define STATUS_FLASH_SUCCESS            0u
#define STATUS_FLASH_INVALID_ARGUMENT   4u
#define STATUS_FLASH_ACCESS_ERROR       103u
#define STATUS_FLASH_UNKNOWN_PROPERTY   106u

/* status_t values returned by the FlexSPI NOR driver. */
#define STATUS_SUCCESS          0u
#define STATUS_FAIL             1u
#define STATUS_INVALID_ARGUMENT 4u

/* flexspi_nor_config_t (fsl_romapi_flexspi.h) field offsets. */
#define NOR_CFG_TAG             0x000u
#define NOR_CFG_SFLASH_A1_SIZE  0x050u
#define NOR_CFG_PAGE_SIZE       0x1C0u
#define NOR_CFG_SECTOR_SIZE     0x1C4u
#define NOR_CFG_BLOCK_SIZE      0x1D0u
#define NOR_CFG_SIZE            0x200u
#define FCB_TAG                 0x42464346u /* "FCFB" */
#define FCB_NOR_OFFSET          0x400u
#define NOR_MAX_PAGE_SIZE       4096u

enum rom_stub {
    STUB_RUN_BOOTLOADER = 0,
    STUB_NOR_INIT,
    STUB_NOR_WAIT_BUSY,
    STUB_NOR_PAGE_PROGRAM,
    STUB_NOR_ERASE_ALL,
    STUB_NOR_ERASE,
    STUB_NOR_ERASE_SECTOR,
    STUB_NOR_ERASE_BLOCK,
    STUB_NOR_READ,
    STUB_NOR_CONFIG_CLOCK,
    STUB_NOR_SET_CLOCK_SOURCE,
    STUB_NOR_GET_CONFIG,
    STUB_NOR_HW_RESET,
    STUB_NOR_XFER,
    STUB_NOR_UPDATE_LUT,
    STUB_NOR_PARTIAL_PROGRAM,
    STUB_COUNT
};

static const char *const stub_names[STUB_COUNT] = {
    "runBootloader", "init", "wait_busy", "page_program", "erase_all",
    "erase", "erase_sector", "erase_block", "read", "config_clock",
    "set_clock_source", "get_config", "hw_reset", "xfer", "update_lut",
    "partial_program"
};

#define STUB_ADDR(n) (RW612_ROM_BASE_S + STUB_OFF + 4u * (mm_u32)(n))

#define ROM_VERSION      ROMAPI_VERSION('K', 1u, 0u, 0u)
#define NOR_DRV_VERSION  ROMAPI_VERSION('F', 1u, 0u, 0u)

static mm_bool romapi_active = MM_FALSE;
static mm_bool romapi_trace = MM_FALSE;
static mm_u8 nor_src[NOR_MAX_PAGE_SIZE];
static mm_u8 nor_page[NOR_MAX_PAGE_SIZE];

static mm_u32 tree_off(void)
{
    /* The SDK and wolfBoot select the tree from CHIP_INFO[3:0]. */
    return ((mm_rw612_chip_rev() & 0x0Fu) == 0u) ? TREE_A0_OFF : TREE_A1_OFF;
}

static mm_u32 legacy_word(mm_u32 off)
{
    switch (off) {
    case 0x00u: return ROMAPI_VERSION('B', 1u, 0u, 0u);
    case 0x04u: return ROMAPI_PTR(LEGACY_STUB_VERSION);
    case 0x08u: return ROMAPI_PTR(LEGACY_STUB_FLASH_INIT);
    case 0x0Cu: return ROMAPI_PTR(LEGACY_STUB_FLASH_GET_PROPERTY);
    default: return 0u;
    }
}

static mm_u32 rom_word(mm_u32 off)
{
    mm_u32 tree = tree_off();
    if (off >= LEGACY_TREE_OFF && off < LEGACY_TREE_OFF + LEGACY_TREE_SIZE) {
        return legacy_word(off - LEGACY_TREE_OFF);
    }
    if (off >= tree && off < tree + 0x20u) {
        switch (off - tree) {
        case 0x00u: return STUB_ADDR(STUB_RUN_BOOTLOADER) | 1u;   /* runBootloader */
        case 0x04u: return ROM_VERSION;                            /* version */
        case 0x08u: return RW612_ROM_BASE_S + COPYRIGHT_OFF;       /* copyright */
        case 0x14u: return RW612_ROM_BASE_S + FLEXSPI_DRV_OFF;     /* flexspiNorDriver */
        default: return 0u; /* nboot, OTP and IAP drivers are not modelled */
        }
    }
    if (off >= FLEXSPI_DRV_OFF && off < FLEXSPI_DRV_OFF + 4u * STUB_COUNT) {
        mm_u32 idx = (off - FLEXSPI_DRV_OFF) / 4u;
        return (idx == 0u) ? NOR_DRV_VERSION : (STUB_ADDR(STUB_NOR_INIT + idx - 1u) | 1u);
    }
    if (off >= COPYRIGHT_OFF && off < COPYRIGHT_OFF + 0x40u) {
        static const char text[] = "m33mu RW612 boot ROM model";
        mm_u32 v = 0;
        mm_u32 i;
        for (i = 0; i < 4u; ++i) {
            mm_u32 idx = off - COPYRIGHT_OFF + i;
            mm_u8 c = (idx < sizeof(text)) ? (mm_u8)text[idx] : 0u;
            v |= (mm_u32)c << (8u * i);
        }
        return v;
    }
    if ((off >= STUB_OFF && off < STUB_OFF + 4u * STUB_COUNT) ||
        (off >= LEGACY_STUB_OFF && off < LEGACY_STUB_OFF + LEGACY_STUB_SIZE)) {
        return 0x47704770u; /* bx lr; bx lr (never executed: calls are trapped) */
    }
    return 0u;
}

static mm_bool rom_read(void *opaque, mm_u32 offset, mm_u32 size, mm_u32 *value_out)
{
    mm_u32 w;
    (void)opaque;
    if (value_out == 0 || size == 0u || size > 4u || offset + size > RW612_ROM_SIZE) {
        return MM_FALSE;
    }
    w = rom_word(offset & ~3u);
    if ((offset & 3u) + size > 4u) {
        w = (w >> ((offset & 3u) * 8u)) | (rom_word((offset & ~3u) + 4u) << ((4u - (offset & 3u)) * 8u));
    } else {
        w >>= (offset & 3u) * 8u;
    }
    if (size == 1u) w &= 0xFFu;
    if (size == 2u) w &= 0xFFFFu;
    *value_out = w;
    return MM_TRUE;
}

static mm_bool rom_write(void *opaque, mm_u32 offset, mm_u32 size, mm_u32 value)
{
    (void)opaque;
    (void)offset;
    (void)size;
    (void)value;
    return MM_FALSE; /* ROM */
}

static void romapi_return(struct mm_cpu *cpu)
{
    if (cpu != 0) cpu->r[15] = cpu->r[14];
}

/* ------------------------------------------------------------------------ */
/* Legacy synthetic flash API                                               */
/* ------------------------------------------------------------------------ */

static mm_u32 legacy_flash_init(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 cfg;
    enum mm_sec_state sec;
    mm_bool ok = MM_TRUE;
    if (cpu == 0 || map == 0) return STATUS_FLASH_INVALID_ARGUMENT;
    cfg = cpu->r[0];
    if (cfg == 0u) return STATUS_FLASH_INVALID_ARGUMENT;
    sec = cpu->sec_state;
    ok &= mm_memmap_write(map, sec, cfg + FC_PFLASH_BLOCK_BASE,  4u, RW612_FLASH_BASE_NS);
    ok &= mm_memmap_write(map, sec, cfg + FC_PFLASH_TOTAL_SIZE,  4u, RW612_FLASH_SIZE);
    ok &= mm_memmap_write(map, sec, cfg + FC_PFLASH_BLOCK_COUNT, 4u, 1u);
    ok &= mm_memmap_write(map, sec, cfg + FC_PFLASH_PAGE_SIZE,   4u, RW612_PFLASH_PAGE_SIZE);
    ok &= mm_memmap_write(map, sec, cfg + FC_PFLASH_SECTOR_SIZE, 4u, RW612_PFLASH_SECTOR_SIZE);
    return ok ? STATUS_FLASH_SUCCESS : STATUS_FLASH_ACCESS_ERROR;
}

static mm_u32 legacy_flash_get_property(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 which, out, val = 0u;
    if (cpu == 0 || map == 0) return STATUS_FLASH_INVALID_ARGUMENT;
    which = cpu->r[1];
    out   = cpu->r[2];
    if (out == 0u) return STATUS_FLASH_INVALID_ARGUMENT;
    switch (which) {
    case FLASH_PROP_SECTOR_SIZE: val = RW612_PFLASH_SECTOR_SIZE; break;
    case FLASH_PROP_TOTAL_SIZE:  val = RW612_FLASH_SIZE;         break;
    case FLASH_PROP_BLOCK_SIZE:  val = RW612_FLASH_SIZE;         break;
    case FLASH_PROP_BLOCK_COUNT: val = 1u;                       break;
    case FLASH_PROP_START_ADDR:  val = RW612_FLASH_BASE_NS;      break;
    case FLASH_PROP_PAGE_SIZE:   val = RW612_PFLASH_PAGE_SIZE;   break;
    case FLASH_PROP_SYS_FREQ:    val = RW612_SYS_FREQ_HZ;        break;
    default: return STATUS_FLASH_UNKNOWN_PROPERTY;
    }
    if (!mm_memmap_write(map, cpu->sec_state, out, 4u, val)) {
        return STATUS_FLASH_ACCESS_ERROR;
    }
    return STATUS_FLASH_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* FlexSPI NOR driver                                                       */
/* ------------------------------------------------------------------------ */

static mm_bool cfg_word(struct mm_cpu *cpu, struct mm_memmap *map, mm_u32 off, mm_u32 *out)
{
    if (cpu->r[1] == 0u) return MM_FALSE;
    return mm_memmap_read(map, cpu->sec_state, cpu->r[1] + off, 4u, out);
}

static mm_bool stack_word(struct mm_cpu *cpu, struct mm_memmap *map, mm_u32 index, mm_u32 *out)
{
    return mm_memmap_read(map, cpu->sec_state, cpu->r[13] + 4u * index, 4u, out);
}

static mm_bool is_pow2(mm_u32 v)
{
    return (v != 0u && (v & (v - 1u)) == 0u) ? MM_TRUE : MM_FALSE;
}

static mm_bool copy_from_cpu(struct mm_cpu *cpu, struct mm_memmap *map, mm_u32 src, mm_u8 *dst, mm_u32 len)
{
    mm_u32 i;
    mm_u32 v;
    for (i = 0; i < len; ) {
        if (((src + i) & 3u) == 0u && len - i >= 4u) {
            if (!mm_memmap_read(map, cpu->sec_state, src + i, 4u, &v)) return MM_FALSE;
            dst[i] = (mm_u8)v;
            dst[i + 1u] = (mm_u8)(v >> 8);
            dst[i + 2u] = (mm_u8)(v >> 16);
            dst[i + 3u] = (mm_u8)(v >> 24);
            i += 4u;
        } else {
            if (!mm_memmap_read(map, cpu->sec_state, src + i, 1u, &v)) return MM_FALSE;
            dst[i] = (mm_u8)v;
            i++;
        }
    }
    return MM_TRUE;
}

/* Erase the units of size `unit` covering [off, off + len). */
static mm_u32 nor_erase_units(mm_u32 off, mm_u32 len, mm_u32 unit)
{
    mm_u32 start;
    mm_u32 end;
    if (!is_pow2(unit) || len == 0u || off > 0xFFFFFFFFu - len) return STATUS_INVALID_ARGUMENT;
    start = off & ~(unit - 1u);
    end = off + len;
    if ((end & (unit - 1u)) != 0u) {
        end = (end | (unit - 1u)) + 1u;
    }
    if (end <= start || end > mm_rw612_nor_size()) return STATUS_INVALID_ARGUMENT;
    return mm_rw612_nor_erase(start, end - start) ? STATUS_SUCCESS : STATUS_FAIL;
}

static mm_u32 nor_init(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 tag = 0u;
    mm_u32 page = 0u;
    mm_u32 sector = 0u;
    if (!cfg_word(cpu, map, NOR_CFG_TAG, &tag) ||
        !cfg_word(cpu, map, NOR_CFG_PAGE_SIZE, &page) ||
        !cfg_word(cpu, map, NOR_CFG_SECTOR_SIZE, &sector)) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (tag != FCB_TAG || !is_pow2(page) || page > NOR_MAX_PAGE_SIZE || !is_pow2(sector)) {
        return STATUS_INVALID_ARGUMENT;
    }
    return STATUS_SUCCESS;
}

static mm_u32 nor_page_program(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 page = 0u;
    mm_u32 dst = cpu->r[2];
    mm_u32 base;
    mm_u32 i;
    if (!cfg_word(cpu, map, NOR_CFG_PAGE_SIZE, &page) || !is_pow2(page) ||
        page > NOR_MAX_PAGE_SIZE || cpu->r[3] == 0u) {
        return STATUS_INVALID_ARGUMENT;
    }
    base = dst & ~(page - 1u);
    if (base >= mm_rw612_nor_size() || page > mm_rw612_nor_size() - base) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (!copy_from_cpu(cpu, map, cpu->r[3], nor_src, page)) {
        return STATUS_INVALID_ARGUMENT;
    }
    /* The NOR wraps a page program around within the addressed page. */
    for (i = 0; i < page; ++i) {
        nor_page[(dst + i) & (page - 1u)] = nor_src[i];
    }
    return mm_rw612_nor_program(base, nor_page, page) ? STATUS_SUCCESS : STATUS_FAIL;
}

static mm_u32 nor_partial_program(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 dst = cpu->r[2];
    mm_u32 len = 0u;
    mm_u32 done = 0u;
    mm_u32 chunk;
    if (cpu->r[3] == 0u || !stack_word(cpu, map, 0u, &len)) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (dst >= mm_rw612_nor_size() || len > mm_rw612_nor_size() - dst) {
        return STATUS_INVALID_ARGUMENT;
    }
    while (done < len) {
        chunk = len - done;
        if (chunk > NOR_MAX_PAGE_SIZE) chunk = NOR_MAX_PAGE_SIZE;
        if (!copy_from_cpu(cpu, map, cpu->r[3] + done, nor_src, chunk) ||
            !mm_rw612_nor_program(dst + done, nor_src, chunk)) {
            return STATUS_FAIL;
        }
        done += chunk;
    }
    return STATUS_SUCCESS;
}

static mm_u32 nor_read(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 dst = cpu->r[2];
    mm_u32 start = cpu->r[3];
    mm_u32 len = 0u;
    mm_u32 i;
    mm_u8 b;
    if (dst == 0u || !stack_word(cpu, map, 0u, &len)) return STATUS_INVALID_ARGUMENT;
    if (start >= mm_rw612_nor_size() || len > mm_rw612_nor_size() - start) {
        return STATUS_INVALID_ARGUMENT;
    }
    for (i = 0; i < len; ++i) {
        if (!mm_rw612_nor_read(start + i, &b, 1u) ||
            !mm_memmap_write(map, cpu->sec_state, dst + i, 1u, b)) {
            return STATUS_FAIL;
        }
    }
    return STATUS_SUCCESS;
}

/* The boot FCB describes the attached NOR, which is what an SFDP probe of
 * the device reports. */
static mm_u32 nor_get_config(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 i;
    mm_u8 b = 0u;
    mm_u8 t[4];
    if (cpu->r[1] == 0u) return STATUS_INVALID_ARGUMENT;
    if (!mm_rw612_nor_read(FCB_NOR_OFFSET, t, 4u)) return STATUS_FAIL;
    if (((mm_u32)t[0] | ((mm_u32)t[1] << 8) | ((mm_u32)t[2] << 16) | ((mm_u32)t[3] << 24)) != FCB_TAG) {
        return STATUS_FAIL;
    }
    for (i = 0; i < NOR_CFG_SIZE; ++i) {
        if (!mm_rw612_nor_read(FCB_NOR_OFFSET + i, &b, 1u) ||
            !mm_memmap_write(map, cpu->sec_state, cpu->r[1] + i, 1u, b)) {
            return STATUS_FAIL;
        }
    }
    return STATUS_SUCCESS;
}

static mm_u32 nor_call(enum rom_stub stub, struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 unit = 0u;
    mm_u32 size = 0u;
    if (stub == STUB_NOR_CONFIG_CLOCK || stub == STUB_NOR_HW_RESET) {
        return cpu->r[0]; /* void: r0 is left as the caller had it */
    }
    if (stub == STUB_NOR_SET_CLOCK_SOURCE) {
        return STATUS_SUCCESS;
    }
    if (cpu->r[0] != 0u) {
        return STATUS_INVALID_ARGUMENT; /* a single FlexSPI instance */
    }
    switch (stub) {
    case STUB_NOR_INIT:
        return nor_init(cpu, map);
    case STUB_NOR_WAIT_BUSY:
    case STUB_NOR_UPDATE_LUT:
        return STATUS_SUCCESS;
    case STUB_NOR_PAGE_PROGRAM:
        return nor_page_program(cpu, map);
    case STUB_NOR_PARTIAL_PROGRAM:
        return nor_partial_program(cpu, map);
    case STUB_NOR_ERASE_SECTOR:
        if (!cfg_word(cpu, map, NOR_CFG_SECTOR_SIZE, &unit)) return STATUS_INVALID_ARGUMENT;
        return nor_erase_units(cpu->r[2], 1u, unit);
    case STUB_NOR_ERASE_BLOCK:
        if (!cfg_word(cpu, map, NOR_CFG_BLOCK_SIZE, &unit)) return STATUS_INVALID_ARGUMENT;
        return nor_erase_units(cpu->r[2], 1u, unit);
    case STUB_NOR_ERASE:
        if (!cfg_word(cpu, map, NOR_CFG_SECTOR_SIZE, &unit)) return STATUS_INVALID_ARGUMENT;
        return nor_erase_units(cpu->r[2], cpu->r[3], unit);
    case STUB_NOR_ERASE_ALL:
        if (!cfg_word(cpu, map, NOR_CFG_SFLASH_A1_SIZE, &size)) return STATUS_INVALID_ARGUMENT;
        if (size == 0u || size > mm_rw612_nor_size()) size = mm_rw612_nor_size();
        return mm_rw612_nor_erase(0u, size) ? STATUS_SUCCESS : STATUS_FAIL;
    case STUB_NOR_READ:
        return nor_read(cpu, map);
    case STUB_NOR_GET_CONFIG:
        return nor_get_config(cpu, map);
    case STUB_NOR_XFER:
    default:
        fprintf(stderr, "[RW612] ROM FlexSPI %s not modelled\n", stub_names[stub]);
        return STATUS_FAIL;
    }
}

mm_bool mm_rw612_romapi_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    const char *env;
    if (bus == 0) return MM_FALSE;
    memset(&reg, 0, sizeof(reg));
    reg.size  = RW612_ROM_SIZE;
    reg.read  = rom_read;
    reg.write = rom_write;
    reg.base  = RW612_ROM_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base  = RW612_ROM_BASE_NS;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    env = getenv("M33MU_ROMAPI_TRACE");
    romapi_trace = (env != 0 && env[0] != '\0' && strcmp(env, "0") != 0) ? MM_TRUE : MM_FALSE;
    romapi_active = MM_TRUE;
    return MM_TRUE;
}

void mm_rw612_romapi_reset(void)
{
}

mm_bool mm_rw612_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 pc;
    enum rom_stub stub;
    if (!romapi_active || cpu == 0 || map == 0) return MM_FALSE;
    pc = cpu->r[15] & ~1u;
    if (pc >= RW612_ROM_BASE_NS && pc < RW612_ROM_BASE_NS + RW612_ROM_SIZE) {
        pc += ROM_ALIAS_DELTA;
    }
    switch (pc) {
    case LEGACY_STUB_FLASH_INIT + ROM_ALIAS_DELTA:
        cpu->r[0] = legacy_flash_init(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case LEGACY_STUB_FLASH_GET_PROPERTY + ROM_ALIAS_DELTA:
        cpu->r[0] = legacy_flash_get_property(cpu, map);
        romapi_return(cpu);
        return MM_TRUE;
    case LEGACY_STUB_VERSION + ROM_ALIAS_DELTA:
        cpu->r[0] = ROMAPI_VERSION('R', 1u, 0u, 0u);
        romapi_return(cpu);
        return MM_TRUE;
    default:
        break;
    }
    if (pc < STUB_ADDR(0) || pc >= STUB_ADDR(STUB_COUNT)) return MM_FALSE;
    stub = (enum rom_stub)((pc - STUB_ADDR(0)) / 4u);
    if (romapi_trace) {
        printf("[ROMAPI] rw612 %s r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx\n",
               stub_names[stub], (unsigned long)cpu->r[0], (unsigned long)cpu->r[1],
               (unsigned long)cpu->r[2], (unsigned long)cpu->r[3]);
    }
    if (stub == STUB_RUN_BOOTLOADER) {
        /* ISP is not modelled: report and return to the caller. */
        fprintf(stderr, "[RW612] runBootloader(0x%08lx) requested: ISP not emulated\n",
                (unsigned long)cpu->r[0]);
        romapi_return(cpu);
        return MM_TRUE;
    }
    cpu->r[0] = nor_call(stub, cpu, map);
    if (romapi_trace) {
        printf("[ROMAPI] rw612 %s -> %lu\n", stub_names[stub], (unsigned long)cpu->r[0]);
    }
    romapi_return(cpu);
    return MM_TRUE;
}

/* Another target was selected in this process. */
void mm_rw612_romapi_deactivate(void)
{
    romapi_active = MM_FALSE;
}
