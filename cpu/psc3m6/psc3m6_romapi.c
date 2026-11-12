/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * PSOC Control C3 boot ROM flash API.
 *
 * The 64 KB boot ROM (0x00800000 NS / 0x10800000 S) is modelled as a
 * read-only window holding the rom_funct_list_t function table at 0x1080FF6C
 * (cyboot_flash_list.h) and one stub-execution slot per entry.  The three
 * blocking row operations the PDL and wolfBoot use are serviced:
 *
 *   cyboot_flash_erase_row(addr, ctx)           -> row to 0x00
 *   cyboot_flash_program_row(addr, data, ctx)   -> OR data into the row
 *   cyboot_flash_write_row(addr, data, ctx)     -> erase then program
 *
 * Addresses must be the SBUS alias of the flash (0x32xxxxxx Secure or
 * 0x22xxxxxx Non-secure) and row aligned; the CBUS view the code runs from
 * is refused with CYBOOT_FLASH_ADDR_INVALID, as on silicon, and so is the
 * Secure alias from a Non-secure caller.  Every other
 * entry returns CYBOOT_FLASH_FAILED so a caller never branches into the void.
 * The ROM writes the refresh bookkeeping through ctx->refresh for the row's
 * 128 KB sector; a NULL or unmapped pointer fails the call.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psc3m6/psc3m6_romapi.h"
#include "psc3m6/cpu_config.h"
#include "m33mu/code_cache.h"

#define ROM_FUNC_OFFSET  (PSC3M6_ROM_FUNC_TABLE - PSC3M6_ROM_BASE_S)   /* 0xFF6C */
#define ROM_STUB_OFFSET  0x1000u
#define ROM_STUB_STRIDE  0x10u
#define ROM_STUB_COUNT   4u

#define STUB_UNSUPPORTED 0u
#define STUB_ERASE_ROW   1u
#define STUB_PROGRAM_ROW 2u
#define STUB_WRITE_ROW   3u

#define CTX_FLAGS        0x0u
#define CTX_REFRESH      0x8u
#define CY_FLASH_MODE_NONBLOCKING (1u << 0)
#define REFRESH_ENTRY_SIZE 16u
#define REFRESH_MAX_COUNT  0x4u

struct romapi_state {
    mm_u8 rom[PSC3M6_ROM_SIZE];
    struct mm_memmap *map;
    mm_u8 *flash;
    mm_u32 flash_size;
    const struct mm_flash_persist *persist;
    mm_bool active;
};

static struct romapi_state romapi;

static int romapi_trace(void)
{
    static int level = -1;
    if (level < 0) {
        const char *env = getenv("M33MU_ROMAPI_TRACE");
        level = (env != 0 && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return level;
}

static void rom_write32(mm_u32 offset, mm_u32 value)
{
    if (offset + 4u > PSC3M6_ROM_SIZE) return;
    romapi.rom[offset] = (mm_u8)(value & 0xFFu);
    romapi.rom[offset + 1u] = (mm_u8)((value >> 8) & 0xFFu);
    romapi.rom[offset + 2u] = (mm_u8)((value >> 16) & 0xFFu);
    romapi.rom[offset + 3u] = (mm_u8)((value >> 24) & 0xFFu);
}

static mm_u32 stub_addr(mm_u32 stub)
{
    return PSC3M6_ROM_BASE_S + ROM_STUB_OFFSET + stub * ROM_STUB_STRIDE;
}

static void romapi_build_table(void)
{
    mm_u32 i;
    memset(romapi.rom, 0, sizeof(romapi.rom));
    /* The stub slots hold BKPT #0xBE so a fetch there is loud; the run loop
     * services the call before any fetch happens. */
    memset(romapi.rom + ROM_STUB_OFFSET, 0xBEu, ROM_STUB_COUNT * ROM_STUB_STRIDE);
    for (i = 0; i < PSC3M6_ROM_FUNC_COUNT; ++i) {
        mm_u32 stub = STUB_UNSUPPORTED;
        if (i == PSC3M6_ROM_FUNC_ERASE_ROW) stub = STUB_ERASE_ROW;
        if (i == PSC3M6_ROM_FUNC_PROGRAM_ROW) stub = STUB_PROGRAM_ROW;
        if (i == PSC3M6_ROM_FUNC_WRITE_ROW) stub = STUB_WRITE_ROW;
        rom_write32(ROM_FUNC_OFFSET + i * 4u, stub_addr(stub) | 1u);
    }
}

static mm_bool rom_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    mm_u32 v = 0u, i;
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if (offset + size_bytes > PSC3M6_ROM_SIZE) return MM_FALSE;
    for (i = 0; i < size_bytes; ++i) {
        v |= ((mm_u32)romapi.rom[offset + i]) << (8u * i);
    }
    *value_out = v;
    return MM_TRUE;
}

static mm_bool rom_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque; (void)offset; (void)size_bytes; (void)value;
    return MM_FALSE;
}

mm_bool mm_psc3m6_romapi_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    memset(&reg, 0, sizeof(reg));
    reg.size = PSC3M6_ROM_SIZE;
    reg.read = rom_read;
    reg.write = rom_write;
    reg.base = PSC3M6_ROM_BASE_S;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = PSC3M6_ROM_BASE_NS;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    romapi.active = MM_TRUE;
    romapi_build_table();
    return MM_TRUE;
}

void mm_psc3m6_romapi_reset(void)
{
    if (!romapi.active) return;
    romapi_build_table();
}

void mm_psc3m6_romapi_bind(struct mm_memmap *map, mm_u8 *flash, mm_u32 flash_size,
                           const struct mm_flash_persist *persist)
{
    romapi.map = map;
    romapi.flash = flash;
    romapi.flash_size = flash_size;
    romapi.persist = persist;
}

static void romapi_return(struct mm_cpu *cpu, mm_u32 status)
{
    cpu->r[0] = status;
    cpu->r[15] = cpu->r[14];
}

/* SBUS flash address -> byte offset into the flash array.  Bit 28 selects
 * the Secure alias, which a Non-secure caller may not name. */
static mm_bool flash_offset_of(mm_u32 addr, enum mm_sec_state sec, mm_u32 *offset_out)
{
    mm_u32 base;
    if (addr >= PSC3M6_FLASH_BASE_S2 && addr < PSC3M6_FLASH_BASE_S2 + romapi.flash_size) {
        if (sec != MM_SECURE) return MM_FALSE;
        base = PSC3M6_FLASH_BASE_S2;
    } else if (addr >= PSC3M6_FLASH_BASE_NS2 && addr < PSC3M6_FLASH_BASE_NS2 + romapi.flash_size) {
        base = PSC3M6_FLASH_BASE_NS2;
    } else {
        return MM_FALSE;
    }
    *offset_out = addr - base;
    return MM_TRUE;
}

static void flash_note_write(mm_u32 offset, mm_u32 len)
{
    if (romapi.persist != 0 && romapi.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)romapi.persist, offset, len);
    }
    if (romapi.map != 0 && romapi.map->code_cache != 0) {
        mm_code_cache_note_write(romapi.map->code_cache, PSC3M6_FLASH_BASE_S + offset, len);
        mm_code_cache_note_write(romapi.map->code_cache, PSC3M6_FLASH_BASE_NS + offset, len);
        mm_code_cache_note_write(romapi.map->code_cache, PSC3M6_FLASH_BASE_S2 + offset, len);
        mm_code_cache_note_write(romapi.map->code_cache, PSC3M6_FLASH_BASE_NS2 + offset, len);
    }
}

/* Validate the context and account the sector in ctx->refresh. */
static mm_u32 romapi_ctx_touch(struct mm_cpu *cpu, struct mm_memmap *map, mm_u32 ctx,
                               mm_u32 offset)
{
    mm_u32 flags = 0u;
    mm_u32 refresh = 0u;
    mm_u32 entry;
    mm_u32 count = 0u;
    if (ctx == 0u) return CYBOOT_FLASH_PARAM_INVALID;
    if (!mm_memmap_read(map, cpu->sec_state, ctx + CTX_FLAGS, 4u, &flags)) {
        return CYBOOT_FLASH_PARAM_INVALID;
    }
    if ((flags & CY_FLASH_MODE_NONBLOCKING) != 0u) {
        return CYBOOT_FLASH_PARAM_INVALID;
    }
    if (!mm_memmap_read(map, cpu->sec_state, ctx + CTX_REFRESH, 4u, &refresh)) {
        return CYBOOT_FLASH_PARAM_INVALID;
    }
    entry = refresh + (offset / PSC3M6_FLASH_SECTOR_SIZE) * REFRESH_ENTRY_SIZE;
    if (!mm_memmap_read(map, cpu->sec_state, entry + REFRESH_MAX_COUNT, 4u, &count) ||
        !mm_memmap_write(map, cpu->sec_state, entry + REFRESH_MAX_COUNT, 4u, count + 1u)) {
        return CYBOOT_FLASH_FAILED;
    }
    return CYBOOT_FLASH_SUCCESS;
}

static mm_u32 romapi_row_op(struct mm_cpu *cpu, struct mm_memmap *map, mm_u32 stub)
{
    mm_u32 addr = cpu->r[0];
    mm_u32 data = 0u;
    mm_u32 ctx;
    mm_u32 offset;
    mm_u32 status;
    mm_u8 row[PSC3M6_FLASH_ROW_SIZE];
    mm_u32 i;

    if (stub == STUB_ERASE_ROW) {
        ctx = cpu->r[1];
    } else {
        data = cpu->r[1];
        ctx = cpu->r[2];
    }
    if (romapi.flash == 0) return CYBOOT_FLASH_FAILED;
    if (!flash_offset_of(addr, cpu->sec_state, &offset) || (offset % PSC3M6_FLASH_ROW_SIZE) != 0u ||
        offset + PSC3M6_FLASH_ROW_SIZE > romapi.flash_size) {
        return CYBOOT_FLASH_ADDR_INVALID;
    }
    /* Stage the whole row first so a bad buffer leaves the bookkeeping alone. */
    if (stub != STUB_ERASE_ROW) {
        if (data == 0u) return CYBOOT_FLASH_PARAM_INVALID;
        for (i = 0; i < PSC3M6_FLASH_ROW_SIZE; i += 4u) {
            mm_u32 w = 0u;
            if (!mm_memmap_read(map, cpu->sec_state, data + i, 4u, &w)) {
                return CYBOOT_FLASH_PARAM_INVALID;
            }
            row[i] = (mm_u8)(w & 0xFFu);
            row[i + 1u] = (mm_u8)((w >> 8) & 0xFFu);
            row[i + 2u] = (mm_u8)((w >> 16) & 0xFFu);
            row[i + 3u] = (mm_u8)((w >> 24) & 0xFFu);
        }
    }
    status = romapi_ctx_touch(cpu, map, ctx, offset);
    if (status != CYBOOT_FLASH_SUCCESS) return status;
    switch (stub) {
    case STUB_ERASE_ROW:
        memset(romapi.flash + offset, 0x00, PSC3M6_FLASH_ROW_SIZE);
        break;
    case STUB_PROGRAM_ROW:
        /* Erased cells read 0; programming only sets bits. */
        for (i = 0; i < PSC3M6_FLASH_ROW_SIZE; ++i) {
            romapi.flash[offset + i] |= row[i];
        }
        break;
    default:
        memcpy(romapi.flash + offset, row, PSC3M6_FLASH_ROW_SIZE);
        break;
    }
    flash_note_write(offset, PSC3M6_FLASH_ROW_SIZE);
    return CYBOOT_FLASH_SUCCESS;
}

mm_bool mm_psc3m6_romapi_handle(struct mm_cpu *cpu, struct mm_memmap *map)
{
    mm_u32 pc;
    mm_u32 stub;
    mm_u32 status;
    if (!romapi.active || cpu == 0 || map == 0) return MM_FALSE;
    pc = cpu->r[15] & ~1u;
    /* Either alias of the stub slots: the S view differs only in bit 28. */
    if ((pc & ~0x10000000u) < PSC3M6_ROM_BASE_NS + ROM_STUB_OFFSET ||
        (pc & ~0x10000000u) >= PSC3M6_ROM_BASE_NS + ROM_STUB_OFFSET + ROM_STUB_COUNT * ROM_STUB_STRIDE) {
        return MM_FALSE;
    }
    stub = ((pc & ~0x10000000u) - (PSC3M6_ROM_BASE_NS + ROM_STUB_OFFSET)) / ROM_STUB_STRIDE;
    switch (stub) {
    case STUB_ERASE_ROW:
    case STUB_PROGRAM_ROW:
    case STUB_WRITE_ROW:
        status = romapi_row_op(cpu, map, stub);
        break;
    default:
        status = CYBOOT_FLASH_FAILED;
        break;
    }
    if (romapi_trace()) {
        static const char *names[ROM_STUB_COUNT] = {
            "unsupported", "erase_row", "program_row", "write_row"
        };
        printf("[ROMAPI] psc3m6 %s addr=0x%08lx -> 0x%08lx\n",
               names[stub], (unsigned long)cpu->r[0], (unsigned long)status);
    }
    romapi_return(cpu, status);
    return MM_TRUE;
}
