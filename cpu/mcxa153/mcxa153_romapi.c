/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "mcxa153/mcxa153_romapi.h"

/* fsl_romapi.h (MCXA153): bootloader_tree_t at ROM_API_BASE, with the
 * flash_driver_interface_t it points to placed just below it. */
#define ROMAPI_TREE_ADDR  0x03003FE0u
#define ROMAPI_FLASH_ADDR 0x03003F80u
#define ROMAPI_STUB_BASE  MCXA153_ROMAPI_BASE

#define ROMAPI_STUB_RUN_BOOTLOADER      (ROMAPI_STUB_BASE + 0x000u)
#define ROMAPI_STUB_JUMP                (ROMAPI_STUB_BASE + 0x010u)
#define ROMAPI_STUB_FLASH_INIT          (ROMAPI_STUB_BASE + 0x020u)
#define ROMAPI_STUB_FLASH_ERASE_SECTOR  (ROMAPI_STUB_BASE + 0x030u)
#define ROMAPI_STUB_FLASH_PROGRAM_PHRASE (ROMAPI_STUB_BASE + 0x040u)
#define ROMAPI_STUB_FLASH_PROGRAM_PAGE  (ROMAPI_STUB_BASE + 0x050u)
#define ROMAPI_STUB_FLASH_VERIFY_PROGRAM (ROMAPI_STUB_BASE + 0x060u)
#define ROMAPI_STUB_FLASH_VERIFY_ERASE_PHRASE (ROMAPI_STUB_BASE + 0x070u)
#define ROMAPI_STUB_FLASH_VERIFY_ERASE_PAGE (ROMAPI_STUB_BASE + 0x080u)
#define ROMAPI_STUB_FLASH_VERIFY_ERASE_SECTOR (ROMAPI_STUB_BASE + 0x090u)
#define ROMAPI_STUB_FLASH_GET_PROPERTY  (ROMAPI_STUB_BASE + 0x0A0u)
#define ROMAPI_STUB_IFR_VERIFY_ERASE_PHRASE (ROMAPI_STUB_BASE + 0x0B0u)
#define ROMAPI_STUB_IFR_VERIFY_ERASE_PAGE (ROMAPI_STUB_BASE + 0x0C0u)
#define ROMAPI_STUB_IFR_VERIFY_ERASE_SECTOR (ROMAPI_STUB_BASE + 0x0D0u)
#define ROMAPI_STUB_FLASH_READ          (ROMAPI_STUB_BASE + 0x0E0u)

#define ROMAPI_PTR(addr) ((mm_u32)(addr) | 1u)

/* flash_driver_interface_t offsets */
#define FLASH_DRV_INIT             0x00u
#define FLASH_DRV_ERASE_SECTOR     0x04u
#define FLASH_DRV_PROGRAM_PHRASE   0x08u
#define FLASH_DRV_PROGRAM_PAGE     0x0Cu
#define FLASH_DRV_VERIFY_PROGRAM   0x10u
#define FLASH_DRV_VERIFY_ERASE_PHRASE 0x14u
#define FLASH_DRV_VERIFY_ERASE_PAGE 0x18u
#define FLASH_DRV_VERIFY_ERASE_SECTOR 0x1Cu
#define FLASH_DRV_GET_PROPERTY     0x20u
#define FLASH_DRV_IFR_VERIFY_ERASE_PHRASE 0x24u
#define FLASH_DRV_IFR_VERIFY_ERASE_PAGE 0x28u
#define FLASH_DRV_IFR_VERIFY_ERASE_SECTOR 0x2Cu
#define FLASH_DRV_READ             0x30u
#define FLASH_DRV_VERSION          0x34u
#define FLASH_DRV_VERSION_VALUE    0x46020001u

/* flash_config_t offsets */
#define FLASH_CFG_BLOCK_BASE   0x00u
#define FLASH_CFG_TOTAL_SIZE   0x04u
#define FLASH_CFG_BLOCK_COUNT  0x08u
#define FLASH_CFG_PAGE_SIZE    0x0Cu
#define FLASH_CFG_SECTOR_SIZE  0x10u
#define FLASH_CFG_FFR_BLOCK_BASE 0x14u
#define FLASH_CFG_FFR_TOTAL_SIZE 0x18u
#define FLASH_CFG_FFR_PAGE_SIZE 0x1Cu
#define FLASH_CFG_FFR_SECTOR_SIZE 0x20u
#define FLASH_CFG_FFR_CFPA_VERSION 0x24u
#define FLASH_CFG_FFR_CFPA_OFFSET 0x28u

#define PFLASH_BLOCK_BASE  0x00000000u
#define PFLASH_TOTAL_SIZE  0x00020000u
#define PFLASH_BLOCK_COUNT 1u
#define PFLASH_PAGE_SIZE   128u
#define PFLASH_SECTOR_SIZE 0x2000u
#define PFLASH_SYS_FREQ_HZ 96000000u
#define IFR0_BLOCK_BASE    0x01000000u
#define IFR0_TOTAL_SIZE    0x00002000u

#define ROMAPI_ERASE_KEY   0x6B65666Cu

/* kStatus_FLASH_* from fsl_romapi.h */
#define STATUS_SUCCESS          0u
#define STATUS_INVALID_ARGUMENT 4u
#define STATUS_SIZE_ERROR       100u
#define STATUS_ALIGNMENT_ERROR  101u
#define STATUS_ADDRESS_ERROR    102u
#define STATUS_ACCESS_ERROR     103u
#define STATUS_COMMAND_FAILURE  105u
#define STATUS_UNKNOWN_PROPERTY 106u
#define STATUS_ERASE_KEY_ERROR  107u

static mm_u8 romapi_buf[MCXA153_ROMAPI_SIZE];
static mm_bool romapi_active = MM_FALSE;
static mm_bool romapi_trace = MM_FALSE;
static struct mm_flash_persist *romapi_persist = 0;

static void romapi_trace_init(void)
{
    const char *env = getenv("M33MU_ROMAPI_TRACE");
    romapi_trace = (env != 0 && env[0] != '\0' && strcmp(env, "0") != 0) ? MM_TRUE : MM_FALSE;
}

static void romapi_put32(mm_u32 addr, mm_u32 value)
{
    mm_u32 offset = addr - MCXA153_ROMAPI_BASE;
    if (addr < MCXA153_ROMAPI_BASE || offset + 4u > MCXA153_ROMAPI_SIZE) return;
    romapi_buf[offset] = (mm_u8)(value & 0xffu);
    romapi_buf[offset + 1u] = (mm_u8)((value >> 8u) & 0xffu);
    romapi_buf[offset + 2u] = (mm_u8)((value >> 16u) & 0xffu);
    romapi_buf[offset + 3u] = (mm_u8)((value >> 24u) & 0xffu);
}

static void romapi_build_table(void)
{
    memset(romapi_buf, 0, sizeof(romapi_buf));
    romapi_put32(ROMAPI_TREE_ADDR + 0x00u, ROMAPI_PTR(ROMAPI_STUB_RUN_BOOTLOADER));
    romapi_put32(ROMAPI_TREE_ADDR + 0x04u, ROMAPI_FLASH_ADDR);
    romapi_put32(ROMAPI_TREE_ADDR + 0x08u, ROMAPI_PTR(ROMAPI_STUB_JUMP));

    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_INIT, ROMAPI_PTR(ROMAPI_STUB_FLASH_INIT));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_ERASE_SECTOR, ROMAPI_PTR(ROMAPI_STUB_FLASH_ERASE_SECTOR));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_PROGRAM_PHRASE, ROMAPI_PTR(ROMAPI_STUB_FLASH_PROGRAM_PHRASE));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_PROGRAM_PAGE, ROMAPI_PTR(ROMAPI_STUB_FLASH_PROGRAM_PAGE));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_VERIFY_PROGRAM, ROMAPI_PTR(ROMAPI_STUB_FLASH_VERIFY_PROGRAM));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_VERIFY_ERASE_PHRASE, ROMAPI_PTR(ROMAPI_STUB_FLASH_VERIFY_ERASE_PHRASE));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_VERIFY_ERASE_PAGE, ROMAPI_PTR(ROMAPI_STUB_FLASH_VERIFY_ERASE_PAGE));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_VERIFY_ERASE_SECTOR, ROMAPI_PTR(ROMAPI_STUB_FLASH_VERIFY_ERASE_SECTOR));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_GET_PROPERTY, ROMAPI_PTR(ROMAPI_STUB_FLASH_GET_PROPERTY));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_IFR_VERIFY_ERASE_PHRASE, ROMAPI_PTR(ROMAPI_STUB_IFR_VERIFY_ERASE_PHRASE));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_IFR_VERIFY_ERASE_PAGE, ROMAPI_PTR(ROMAPI_STUB_IFR_VERIFY_ERASE_PAGE));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_IFR_VERIFY_ERASE_SECTOR, ROMAPI_PTR(ROMAPI_STUB_IFR_VERIFY_ERASE_SECTOR));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_READ, ROMAPI_PTR(ROMAPI_STUB_FLASH_READ));
    romapi_put32(ROMAPI_FLASH_ADDR + FLASH_DRV_VERSION, FLASH_DRV_VERSION_VALUE);
}

static mm_bool romapi_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u32 v = 0u;
    mm_u32 i;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if (offset + size_bytes > MCXA153_ROMAPI_SIZE) return MM_FALSE;
    for (i = 0; i < size_bytes; ++i) {
        v |= ((mm_u32)romapi_buf[offset + i]) << (8u * i);
    }
    *value_out = v;
    return MM_TRUE;
}

static mm_bool romapi_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    (void)offset;
    (void)size_bytes;
    (void)value;
    return MM_FALSE;
}

static mm_bool romapi_flash_offset(const struct mm_memmap *map, mm_u32 addr, mm_u32 len, mm_u32 *offset_out)
{
    mm_u32 base;
    mm_u32 size;
    if (map == 0 || offset_out == 0 || map->flash.buffer == 0) return MM_FALSE;
    base = map->flash_base_ns;
    size = map->flash_size_ns;
    if (size == 0u) {
        base = map->flash.base;
        size = (mm_u32)map->flash.length;
    }
    if (addr < base || (addr - base) + len > size || (addr - base) + len < len) return MM_FALSE;
    *offset_out = addr - base;
    return MM_TRUE;
}

static void romapi_persist_flush(mm_u32 flash_off, mm_u32 len)
{
    if (romapi_persist != 0) {
        mm_flash_persist_flush(romapi_persist, flash_off, len);
    }
}

static mm_bool romapi_get_stack_u32(struct mm_cpu *cpu, struct mm_memmap *map, mm_u32 index, mm_u32 *value_out)
{
    if (cpu == 0 || map == 0 || value_out == 0) return MM_FALSE;
    return mm_memmap_read(map, cpu->sec_state, cpu->r[13] + index * 4u, 4u, value_out);
}

static mm_u32 romapi_flash_init(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 cfg = cpu->r[0];
    enum mm_sec_state sec = cpu->sec_state;
    mm_bool ok = MM_TRUE;
    if (cfg == 0u) return STATUS_INVALID_ARGUMENT;
    ok &= mm_memmap_write(map, sec, cfg + FLASH_CFG_BLOCK_BASE, 4u, PFLASH_BLOCK_BASE);
    ok &= mm_memmap_write(map, sec, cfg + FLASH_CFG_TOTAL_SIZE, 4u, PFLASH_TOTAL_SIZE);
    ok &= mm_memmap_write(map, sec, cfg + FLASH_CFG_BLOCK_COUNT, 4u, PFLASH_BLOCK_COUNT);
    ok &= mm_memmap_write(map, sec, cfg + FLASH_CFG_PAGE_SIZE, 4u, PFLASH_PAGE_SIZE);
    ok &= mm_memmap_write(map, sec, cfg + FLASH_CFG_SECTOR_SIZE, 4u, PFLASH_SECTOR_SIZE);
    ok &= mm_memmap_write(map, sec, cfg + FLASH_CFG_FFR_BLOCK_BASE, 4u, IFR0_BLOCK_BASE);
    ok &= mm_memmap_write(map, sec, cfg + FLASH_CFG_FFR_TOTAL_SIZE, 4u, IFR0_TOTAL_SIZE);
    ok &= mm_memmap_write(map, sec, cfg + FLASH_CFG_FFR_PAGE_SIZE, 4u, PFLASH_PAGE_SIZE);
    ok &= mm_memmap_write(map, sec, cfg + FLASH_CFG_FFR_SECTOR_SIZE, 4u, PFLASH_SECTOR_SIZE);
    ok &= mm_memmap_write(map, sec, cfg + FLASH_CFG_FFR_CFPA_VERSION, 4u, 0u);
    ok &= mm_memmap_write(map, sec, cfg + FLASH_CFG_FFR_CFPA_OFFSET, 4u, 0u);
    return ok ? STATUS_SUCCESS : STATUS_ACCESS_ERROR;
}

/* FLASH_EraseSector(config, start, lengthInBytes, key): the range must be
 * word aligned; every sector it touches is erased. */
static mm_u32 romapi_flash_erase_sector(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 start = cpu->r[1];
    mm_u32 len = cpu->r[2];
    mm_u32 key = cpu->r[3];
    mm_u32 off;
    mm_u32 first;
    mm_u32 last;
    mm_u8 *flash;
    if (cpu->r[0] == 0u) return STATUS_INVALID_ARGUMENT;
    if (key != ROMAPI_ERASE_KEY) return STATUS_ERASE_KEY_ERROR;
    if (len == 0u) return STATUS_SUCCESS;
    if ((start & 3u) != 0u || (len & 3u) != 0u) return STATUS_ALIGNMENT_ERROR;
    if (!romapi_flash_offset(map, start, len, &off)) return STATUS_ADDRESS_ERROR;
    first = off & ~(PFLASH_SECTOR_SIZE - 1u);
    last = ((off + len - 1u) & ~(PFLASH_SECTOR_SIZE - 1u)) + PFLASH_SECTOR_SIZE;
    flash = (mm_u8 *)map->flash.buffer;
    memset(flash + first, 0xFF, last - first);
    romapi_persist_flush(first, last - first);
    if (romapi_trace) {
        printf("[ROMAPI] erase sectors 0x%08lx..0x%08lx\n",
               (unsigned long)(start - off + first), (unsigned long)(start - off + last));
    }
    return STATUS_SUCCESS;
}

/* Programming clears bits, as the HAL relies on when it rewrites a phrase
 * with more zeros (trailer flags). fsl_romapi.h asks for word-aligned
 * start and length. */
static mm_u32 romapi_flash_program(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 start = cpu->r[1];
    mm_u32 src = cpu->r[2];
    mm_u32 len = cpu->r[3];
    mm_u32 off;
    mm_u32 i;
    mm_u8 b;
    mm_u8 *flash;
    if (cpu->r[0] == 0u || src == 0u) return STATUS_INVALID_ARGUMENT;
    if (len == 0u) return STATUS_SUCCESS;
    if ((start & 3u) != 0u || (len & 3u) != 0u) return STATUS_ALIGNMENT_ERROR;
    if (!romapi_flash_offset(map, start, len, &off)) return STATUS_ADDRESS_ERROR;
    flash = (mm_u8 *)map->flash.buffer;
    for (i = 0; i < len; ++i) {
        if (!mm_memmap_read8(map, cpu->sec_state, src + i, &b)) return STATUS_ACCESS_ERROR;
        flash[off + i] &= b;
    }
    romapi_persist_flush(off, len);
    return STATUS_SUCCESS;
}

static mm_u32 romapi_flash_verify_erase(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 start = cpu->r[1];
    mm_u32 len = cpu->r[2];
    mm_u32 off;
    mm_u32 i;
    const mm_u8 *flash;
    if (cpu->r[0] == 0u) return STATUS_INVALID_ARGUMENT;
    if (len == 0u) return STATUS_SUCCESS;
    if ((start & 3u) != 0u || (len & 3u) != 0u) return STATUS_ALIGNMENT_ERROR;
    if (!romapi_flash_offset(map, start, len, &off)) return STATUS_ADDRESS_ERROR;
    flash = (const mm_u8 *)map->flash.buffer;
    for (i = 0; i < len; ++i) {
        if (flash[off + i] != 0xFFu) return STATUS_COMMAND_FAILURE;
    }
    return STATUS_SUCCESS;
}

static mm_u32 romapi_flash_verify_program(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 start = cpu->r[1];
    mm_u32 len = cpu->r[2];
    mm_u32 expected = cpu->r[3];
    mm_u32 failed_addr = 0u;
    mm_u32 failed_data = 0u;
    mm_u32 off;
    mm_u32 i;
    mm_u8 b;
    const mm_u8 *flash;
    if (cpu->r[0] == 0u || expected == 0u) return STATUS_INVALID_ARGUMENT;
    if (len == 0u) return STATUS_SUCCESS;
    if ((start & 3u) != 0u || (len & 3u) != 0u) return STATUS_ALIGNMENT_ERROR;
    if (!romapi_flash_offset(map, start, len, &off)) return STATUS_ADDRESS_ERROR;
    (void)romapi_get_stack_u32(cpu, map, 0u, &failed_addr);
    (void)romapi_get_stack_u32(cpu, map, 1u, &failed_data);
    flash = (const mm_u8 *)map->flash.buffer;
    for (i = 0; i < len; ++i) {
        if (!mm_memmap_read8(map, cpu->sec_state, expected + i, &b)) return STATUS_ACCESS_ERROR;
        if (flash[off + i] != b) {
            mm_u32 word_off = (off + i) & ~3u;
            mm_u32 word = (mm_u32)flash[word_off] | ((mm_u32)flash[word_off + 1u] << 8) |
                          ((mm_u32)flash[word_off + 2u] << 16) | ((mm_u32)flash[word_off + 3u] << 24);
            if (failed_addr != 0u) {
                (void)mm_memmap_write(map, cpu->sec_state, failed_addr, 4u, start + i);
            }
            if (failed_data != 0u) {
                (void)mm_memmap_write(map, cpu->sec_state, failed_data, 4u, word);
            }
            return STATUS_COMMAND_FAILURE;
        }
    }
    return STATUS_SUCCESS;
}

static mm_u32 romapi_flash_get_property(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 which = cpu->r[1];
    mm_u32 out = cpu->r[2];
    mm_u32 val;
    if (cpu->r[0] == 0u || out == 0u) return STATUS_INVALID_ARGUMENT;
    switch (which) {
    case 0x00u: val = PFLASH_SECTOR_SIZE; break;
    case 0x01u: val = PFLASH_TOTAL_SIZE; break;
    case 0x02u: val = PFLASH_TOTAL_SIZE; break;
    case 0x03u: val = PFLASH_BLOCK_COUNT; break;
    case 0x04u: val = PFLASH_BLOCK_BASE; break;
    case 0x30u: val = PFLASH_PAGE_SIZE; break;
    case 0x31u: val = PFLASH_SYS_FREQ_HZ; break;
    case 0x40u: val = PFLASH_SECTOR_SIZE; break;
    case 0x41u: val = IFR0_TOTAL_SIZE; break;
    case 0x42u: val = IFR0_BLOCK_BASE; break;
    case 0x43u: val = PFLASH_PAGE_SIZE; break;
    default:
        return STATUS_UNKNOWN_PROPERTY;
    }
    if (!mm_memmap_write(map, cpu->sec_state, out, 4u, val)) return STATUS_ACCESS_ERROR;
    return STATUS_SUCCESS;
}

static mm_u32 romapi_flash_read(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 start = cpu->r[1];
    mm_u32 dest = cpu->r[2];
    mm_u32 len = cpu->r[3];
    mm_u32 off;
    mm_u32 i;
    const mm_u8 *flash;
    if (cpu->r[0] == 0u || dest == 0u) return STATUS_INVALID_ARGUMENT;
    if (len == 0u) return STATUS_SUCCESS;
    if (!romapi_flash_offset(map, start, len, &off)) return STATUS_ADDRESS_ERROR;
    flash = (const mm_u8 *)map->flash.buffer;
    for (i = 0; i < len; ++i) {
        if (!mm_memmap_write8(map, cpu->sec_state, dest + i, flash[off + i])) return STATUS_ACCESS_ERROR;
    }
    return STATUS_SUCCESS;
}

static const char *romapi_stub_name(mm_u32 pc)
{
    switch (pc) {
    case ROMAPI_STUB_RUN_BOOTLOADER: return "run_bootloader";
    case ROMAPI_STUB_JUMP: return "jump";
    case ROMAPI_STUB_FLASH_INIT: return "flash_init";
    case ROMAPI_STUB_FLASH_ERASE_SECTOR: return "flash_erase_sector";
    case ROMAPI_STUB_FLASH_PROGRAM_PHRASE: return "flash_program_phrase";
    case ROMAPI_STUB_FLASH_PROGRAM_PAGE: return "flash_program_page";
    case ROMAPI_STUB_FLASH_VERIFY_PROGRAM: return "flash_verify_program";
    case ROMAPI_STUB_FLASH_VERIFY_ERASE_PHRASE: return "flash_verify_erase_phrase";
    case ROMAPI_STUB_FLASH_VERIFY_ERASE_PAGE: return "flash_verify_erase_page";
    case ROMAPI_STUB_FLASH_VERIFY_ERASE_SECTOR: return "flash_verify_erase_sector";
    case ROMAPI_STUB_FLASH_GET_PROPERTY: return "flash_get_property";
    case ROMAPI_STUB_IFR_VERIFY_ERASE_PHRASE: return "ifr_verify_erase_phrase";
    case ROMAPI_STUB_IFR_VERIFY_ERASE_PAGE: return "ifr_verify_erase_page";
    case ROMAPI_STUB_IFR_VERIFY_ERASE_SECTOR: return "ifr_verify_erase_sector";
    case ROMAPI_STUB_FLASH_READ: return "flash_read";
    default:
        break;
    }
    return 0;
}

mm_bool mm_mcxa153_romapi_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    memset(&reg, 0, sizeof(reg));
    reg.base = MCXA153_ROMAPI_BASE;
    reg.size = MCXA153_ROMAPI_SIZE;
    reg.opaque = 0;
    reg.read = romapi_read;
    reg.write = romapi_write;
    reg.name = "mcxa153_romapi";
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    romapi_active = MM_TRUE;
    mm_mcxa153_romapi_reset();
    return MM_TRUE;
}

void mm_mcxa153_romapi_reset(void)
{
    romapi_trace_init();
    romapi_build_table();
}

void mm_mcxa153_romapi_set_persist(const struct mm_flash_persist *persist)
{
    romapi_persist = (struct mm_flash_persist *)persist;
}

mm_bool mm_mcxa153_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 pc;
    const char *name;
    mm_u32 status;
    if (!romapi_active || cpu == 0 || map == 0) return MM_FALSE;
    pc = cpu->r[15] & ~1u;
    name = romapi_stub_name(pc);
    if (name == 0) return MM_FALSE;
    if (romapi_trace) {
        printf("[ROMAPI] call pc=0x%08lx fn=%s r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx\n",
               (unsigned long)pc, name,
               (unsigned long)cpu->r[0], (unsigned long)cpu->r[1],
               (unsigned long)cpu->r[2], (unsigned long)cpu->r[3]);
    }
    switch (pc) {
    case ROMAPI_STUB_FLASH_INIT:
        status = romapi_flash_init(cpu, map);
        break;
    case ROMAPI_STUB_FLASH_ERASE_SECTOR:
        status = romapi_flash_erase_sector(cpu, map);
        break;
    case ROMAPI_STUB_FLASH_PROGRAM_PHRASE:
    case ROMAPI_STUB_FLASH_PROGRAM_PAGE:
        status = romapi_flash_program(cpu, map);
        break;
    case ROMAPI_STUB_FLASH_VERIFY_PROGRAM:
        status = romapi_flash_verify_program(cpu, map);
        break;
    case ROMAPI_STUB_FLASH_VERIFY_ERASE_PHRASE:
    case ROMAPI_STUB_FLASH_VERIFY_ERASE_PAGE:
    case ROMAPI_STUB_FLASH_VERIFY_ERASE_SECTOR:
        status = romapi_flash_verify_erase(cpu, map);
        break;
    case ROMAPI_STUB_FLASH_GET_PROPERTY:
        status = romapi_flash_get_property(cpu, map);
        break;
    case ROMAPI_STUB_FLASH_READ:
        status = romapi_flash_read(cpu, map);
        break;
    case ROMAPI_STUB_IFR_VERIFY_ERASE_PHRASE:
    case ROMAPI_STUB_IFR_VERIFY_ERASE_PAGE:
    case ROMAPI_STUB_IFR_VERIFY_ERASE_SECTOR:
        status = STATUS_ACCESS_ERROR;
        break;
    default:
        status = STATUS_SUCCESS;
        break;
    }
    if (romapi_trace) {
        printf("[ROMAPI] %s => %lu\n", name, (unsigned long)status);
    }
    cpu->r[0] = status;
    cpu->r[15] = cpu->r[14];
    return MM_TRUE;
}
