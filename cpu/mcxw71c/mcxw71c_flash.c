/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* MCXW716C FMU0: launch from FCCOB0, store the phrase/page to the array while
 * FSTAT.PEWEN is set, then acknowledge PERDY. The single 1 MB block has no
 * read-while-write, so array reads fault while a command is in flight. */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mcxw71c/mcxw71c_flash.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/code_cache.h"
#include "m33mu/flash_persist.h"

#define FMU_FSTAT  0x00u
#define FMU_FCNFG  0x04u
#define FMU_FCTRL  0x08u
#define FMU_FCCOB0 0x10u
#define FMU_FCCOB_COUNT 8u

#define FSTAT_FAIL         (1u << 0)
#define FSTAT_CMDABT       (1u << 2)
#define FSTAT_PVIOL        (1u << 4)
#define FSTAT_ACCERR       (1u << 5)
#define FSTAT_CWSABT       (1u << 6)
#define FSTAT_CCIF         (1u << 7)
#define FSTAT_DFDIF        (1u << 16)
#define FSTAT_PEWEN_PHRASE (1u << 24)
#define FSTAT_PEWEN_PAGE   (2u << 24)
#define FSTAT_PEWEN_MASK   (3u << 24)
#define FSTAT_PERDY        (1u << 31)
#define FSTAT_W1C (FSTAT_CMDABT | FSTAT_PVIOL | FSTAT_ACCERR | FSTAT_CWSABT | FSTAT_DFDIF)

#define CMD_RD1ALL 0x00u
#define CMD_RD1BLK 0x01u
#define CMD_RD1SCR 0x02u
#define CMD_RD1PG  0x03u
#define CMD_RD1PHR 0x04u
#define CMD_PGMPG  0x23u
#define CMD_PGMPHR 0x24u
#define CMD_ERSALL 0x40u
#define CMD_ERSSCR 0x42u

struct fmu_state {
    mm_u32 fstat;
    mm_u32 fcnfg;
    mm_u32 fctrl;
    mm_u32 fccob[FMU_FCCOB_COUNT];
    mm_u32 cmd;
    mm_u32 span;
    mm_u32 target;
    mm_u32 captured;
    mm_bool have_target;
    mm_u8 buf[MCXW71C_FLASH_PAGE_SIZE];
    struct mm_memmap *map;
    mm_u8 *flash;
    mm_u32 flash_size;
    const struct mm_flash_persist *persist;
};

static struct fmu_state fmu;

static mm_bool fmu_trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("M33MU_FLASH_TRACE");
        cached = (env != 0 && env[0] != '\0') ? 1 : 0;
    }
    return cached != 0;
}

static void fmu_changed(struct fmu_state *f, mm_u32 off, mm_u32 len)
{
    if (f->map != 0 && f->map->code_cache != 0) {
        mm_code_cache_note_write(f->map->code_cache, f->map->flash_base_ns + off, len);
        mm_code_cache_note_write(f->map->code_cache, f->map->flash_base_s + off, len);
    }
    if (f->persist != 0 && f->persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)f->persist, off, len);
    }
}

/* Either alias addresses the same array; FCCOB addresses may use both too. */
static mm_bool fmu_offset(const struct fmu_state *f, mm_u32 addr, mm_u32 *off_out)
{
    mm_u32 off = addr & 0x0FFFFFFFu;
    if (f->flash == 0 || off >= f->flash_size) {
        return MM_FALSE;
    }
    *off_out = off;
    return MM_TRUE;
}

static void fmu_complete(struct fmu_state *f, mm_u32 err)
{
    f->fstat &= ~(FSTAT_PEWEN_MASK | FSTAT_PERDY);
    f->fstat |= FSTAT_CCIF | err;
    f->have_target = MM_FALSE;
    f->captured = 0u;
}

static mm_bool fmu_is_erased(const struct fmu_state *f, mm_u32 off, mm_u32 len)
{
    mm_u32 i;
    for (i = 0; i < len; ++i) {
        if (f->flash[off + i] != 0xFFu) {
            return MM_FALSE;
        }
    }
    return MM_TRUE;
}

static void fmu_verify_erase(struct fmu_state *f, mm_u32 unit)
{
    mm_u32 off;
    if (unit == 0u) {
        fmu_complete(f, fmu_is_erased(f, 0u, f->flash_size) ? 0u : FSTAT_FAIL);
        return;
    }
    if (!fmu_offset(f, f->fccob[2], &off) || (off & (unit - 1u)) != 0u ||
        off + unit > f->flash_size) {
        fmu_complete(f, FSTAT_ACCERR);
        return;
    }
    fmu_complete(f, fmu_is_erased(f, off, unit) ? 0u : FSTAT_FAIL);
}

static void fmu_launch(struct fmu_state *f)
{
    /* A launch is ignored until the previous error flags are cleared. */
    if ((f->fstat & (FSTAT_ACCERR | FSTAT_PVIOL)) != 0u) {
        return;
    }
    f->fstat &= ~(FSTAT_CCIF | FSTAT_FAIL | FSTAT_PEWEN_MASK | FSTAT_PERDY);
    f->cmd = f->fccob[0] & 0xFFu;
    f->have_target = MM_FALSE;
    f->captured = 0u;
    memset(f->buf, 0xFF, sizeof(f->buf));
    if (fmu_trace_enabled()) {
        printf("[FMU] launch cmd=0x%02lx fccob2=0x%08lx\n",
               (unsigned long)f->cmd, (unsigned long)f->fccob[2]);
    }
    if (f->flash == 0) {
        fmu_complete(f, FSTAT_ACCERR);
        return;
    }
    switch (f->cmd) {
    case CMD_PGMPHR:
    case CMD_ERSSCR:
        f->span = MCXW71C_FLASH_PHRASE_SIZE;
        f->fstat |= FSTAT_PEWEN_PHRASE;
        break;
    case CMD_PGMPG:
        f->span = MCXW71C_FLASH_PAGE_SIZE;
        f->fstat |= FSTAT_PEWEN_PAGE;
        break;
    case CMD_ERSALL:
        memset(f->flash, 0xFF, f->flash_size);
        fmu_changed(f, 0u, f->flash_size);
        fmu_complete(f, 0u);
        break;
    case CMD_RD1ALL:
    case CMD_RD1BLK:
        fmu_verify_erase(f, 0u);
        break;
    case CMD_RD1SCR:
        fmu_verify_erase(f, MCXW71C_FLASH_SECTOR_SIZE);
        break;
    case CMD_RD1PG:
        fmu_verify_erase(f, MCXW71C_FLASH_PAGE_SIZE);
        break;
    case CMD_RD1PHR:
        fmu_verify_erase(f, MCXW71C_FLASH_PHRASE_SIZE);
        break;
    default:
        fmu_complete(f, FSTAT_ACCERR);
        break;
    }
}

static void fmu_execute(struct fmu_state *f)
{
    mm_u32 base;
    mm_u32 i;

    if (f->cmd == CMD_ERSSCR) {
        base = f->target & ~(MCXW71C_FLASH_SECTOR_SIZE - 1u);
        memset(f->flash + base, 0xFF, MCXW71C_FLASH_SECTOR_SIZE);
        fmu_changed(f, base, MCXW71C_FLASH_SECTOR_SIZE);
        if (fmu_trace_enabled()) {
            printf("[FMU] erase sector 0x%08lx\n", (unsigned long)base);
        }
    } else {
        if (fmu_trace_enabled() && !fmu_is_erased(f, f->target, f->span)) {
            printf("[FMU] program over non-erased 0x%08lx (ECC phrase)\n",
                   (unsigned long)f->target);
        }
        /* Program only clears bits */
        for (i = 0; i < f->span; ++i) {
            f->flash[f->target + i] &= f->buf[i];
        }
        fmu_changed(f, f->target, f->span);
        if (fmu_trace_enabled()) {
            printf("[FMU] program 0x%08lx len=%lu\n",
                   (unsigned long)f->target, (unsigned long)f->span);
        }
    }
    fmu_complete(f, 0u);
}

static mm_bool fmu_flash_write(void *opaque, enum mm_sec_state sec, mm_u32 addr,
                               mm_u32 size_bytes, mm_u32 value)
{
    struct fmu_state *f = (struct fmu_state *)opaque;
    mm_u32 off;
    mm_u32 words;
    mm_u32 full;
    mm_u32 idx;
    (void)sec;

    if (f == 0 || !fmu_offset(f, addr, &off)) {
        return MM_FALSE;
    }
    /* Outside a write phase the array is read-only: the store bus-faults. */
    if ((f->fstat & FSTAT_PEWEN_MASK) == 0u) {
        if (fmu_trace_enabled()) {
            printf("[FMU] store to 0x%08lx without PEWEN\n", (unsigned long)addr);
        }
        return MM_FALSE;
    }
    if (size_bytes != 4u || (off & 3u) != 0u) {
        fmu_complete(f, FSTAT_ACCERR);
        return MM_TRUE;
    }
    if (!f->have_target) {
        f->target = off & ~(f->span - 1u);
        f->have_target = MM_TRUE;
    } else if ((off & ~(f->span - 1u)) != f->target) {
        fmu_complete(f, FSTAT_ACCERR);
        return MM_TRUE;
    }
    idx = (off - f->target) / 4u;
    f->buf[idx * 4u] = (mm_u8)(value & 0xFFu);
    f->buf[idx * 4u + 1u] = (mm_u8)((value >> 8) & 0xFFu);
    f->buf[idx * 4u + 2u] = (mm_u8)((value >> 16) & 0xFFu);
    f->buf[idx * 4u + 3u] = (mm_u8)((value >> 24) & 0xFFu);
    f->captured |= 1u << idx;
    words = f->span / 4u;
    full = (words >= 32u) ? 0xFFFFFFFFu : ((1u << words) - 1u);
    if (f->captured == full) {
        f->fstat &= ~FSTAT_PEWEN_MASK;
        f->fstat |= FSTAT_PERDY;
    }
    return MM_TRUE;
}

static mm_bool fmu_flash_readable(void *opaque, mm_u32 byte_offset)
{
    const struct fmu_state *f = (const struct fmu_state *)opaque;
    (void)byte_offset;
    if (f == 0) {
        return MM_TRUE;
    }
    return ((f->fstat & FSTAT_CCIF) != 0u) ? MM_TRUE : MM_FALSE;
}

static mm_bool fmu_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct fmu_state *f = (struct fmu_state *)opaque;
    mm_u32 v;
    if (f == 0 || value_out == 0 || size_bytes != 4u || (offset & 3u) != 0u) return MM_FALSE;
    if (offset >= MCXW71C_FMU0_SIZE) return MM_FALSE;
    switch (offset) {
    case FMU_FSTAT: v = f->fstat; break;
    case FMU_FCNFG: v = f->fcnfg; break;
    case FMU_FCTRL: v = f->fctrl; break;
    default:
        if (offset >= FMU_FCCOB0) {
            v = f->fccob[(offset - FMU_FCCOB0) / 4u];
        } else {
            v = 0u;
        }
        break;
    }
    *value_out = v;
    return MM_TRUE;
}

static mm_bool fmu_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct fmu_state *f = (struct fmu_state *)opaque;
    if (f == 0 || size_bytes != 4u || (offset & 3u) != 0u) return MM_FALSE;
    if (offset >= MCXW71C_FMU0_SIZE) return MM_FALSE;
    switch (offset) {
    case FMU_FSTAT:
        f->fstat &= ~(value & FSTAT_W1C);
        if ((value & FSTAT_PERDY) != 0u && (f->fstat & FSTAT_PERDY) != 0u) {
            fmu_execute(f);
        } else if ((value & FSTAT_CCIF) != 0u && (f->fstat & FSTAT_CCIF) != 0u) {
            fmu_launch(f);
        }
        break;
    case FMU_FCNFG:
        f->fcnfg = value;
        break;
    case FMU_FCTRL:
        f->fctrl = value;
        break;
    default:
        /* FCCOB is frozen while a command is in flight. */
        if (offset >= FMU_FCCOB0 && (f->fstat & FSTAT_CCIF) != 0u) {
            f->fccob[(offset - FMU_FCCOB0) / 4u] = value;
        }
        break;
    }
    return MM_TRUE;
}

void mm_mcxw71c_fmu_reset(void)
{
    struct mm_memmap *map = fmu.map;
    mm_u8 *flash = fmu.flash;
    mm_u32 flash_size = fmu.flash_size;
    const struct mm_flash_persist *persist = fmu.persist;

    memset(&fmu, 0, sizeof(fmu));
    fmu.fstat = FSTAT_CCIF;
    fmu.map = map;
    fmu.flash = flash;
    fmu.flash_size = flash_size;
    fmu.persist = persist;
}

mm_bool mm_mcxw71c_fmu_register(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    memset(&reg, 0, sizeof(reg));
    reg.base = MCXW71C_FMU0_BASE;
    reg.size = MCXW71C_FMU0_SIZE;
    reg.opaque = &fmu;
    reg.read = fmu_read;
    reg.write = fmu_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = MCXW71C_FMU0_SEC_BASE;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    return MM_TRUE;
}

void mm_mcxw71c_fmu_bind(struct mm_memmap *map,
                         mm_u8 *flash,
                         mm_u32 flash_size,
                         const struct mm_flash_persist *persist)
{
    fmu.map = map;
    fmu.flash = flash;
    fmu.flash_size = flash_size;
    fmu.persist = persist;
    if (map != 0) {
        mm_memmap_set_flash_writer(map, fmu_flash_write, &fmu);
        mm_memmap_set_flash_ecc_check(map, fmu_flash_readable, &fmu);
    }
}
