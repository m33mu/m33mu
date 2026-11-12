/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * RW612 FlexSPI controller and the serial NOR behind it.
 *
 * The NOR is executed in place through the FlexSPI AHB window
 * (0x08000000 / 0x18000000), which reads straight from the flash backing
 * store.  Like on silicon, the window is read-only to the CPU: software
 * programs and erases the NOR through the boot ROM FlexSPI NOR driver
 * (rw612_romapi.c), which lands here.
 *
 * The controller registers are a register file with the self-clearing bits
 * firmware relies on (MCR0.SWRESET, AHBCR.CLRAHBRXBUF/CLRAHBTXBUF) and an
 * always-idle status, so SDK FlexSPI probes do not spin.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rw612/rw612_flexspi.h"
#include "rw612/cpu_config.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/code_cache.h"
#include "m33mu/flash_persist.h"

#define FLEXSPI_BASE_NS      0x40134000u
#define FLEXSPI_SIZE         0x1000u

#define FLEXSPI_MCR0         0x00u
#define FLEXSPI_AHBCR        0x0Cu
#define FLEXSPI_INTR         0x14u
#define FLEXSPI_STS0         0xE0u

#define MCR0_SWRESET         (1u << 0)
#define AHBCR_CLRAHBRXBUF    (1u << 1)
#define AHBCR_CLRAHBTXBUF    (1u << 2)
#define INTR_IPCMDDONE       (1u << 0)
#define STS0_SEQIDLE         (1u << 0)
#define STS0_ARBIDLE         (1u << 1)

struct flexspi_state {
    mm_u32 regs[FLEXSPI_SIZE / 4u];
    struct mm_memmap *map;
    mm_u8 *flash;
    mm_u32 flash_size;
    const struct mm_flash_persist *persist;
    mm_bool store_warned;
};

static struct flexspi_state fspi;

static mm_bool trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("M33MU_FLEXSPI_TRACE") != 0 ? 1 : 0;
    }
    return cached != 0;
}

static mm_bool flexspi_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if (offset + size_bytes > FLEXSPI_SIZE) return MM_FALSE;
    *value_out = 0u;
    memcpy(value_out, (const mm_u8 *)fspi.regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool flexspi_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0u || size_bytes > 4u) return MM_FALSE;
    if (offset + size_bytes > FLEXSPI_SIZE) return MM_FALSE;
    if (size_bytes == 4u) {
        switch (offset) {
        case FLEXSPI_MCR0:
            fspi.regs[FLEXSPI_MCR0 / 4u] = value & ~MCR0_SWRESET;
            return MM_TRUE;
        case FLEXSPI_AHBCR:
            /* Clearing the AHB buffers completes at once: the model has no
             * prefetch buffer that could hold stale NOR data. */
            fspi.regs[FLEXSPI_AHBCR / 4u] = value & ~(AHBCR_CLRAHBRXBUF | AHBCR_CLRAHBTXBUF);
            return MM_TRUE;
        case FLEXSPI_INTR:
            fspi.regs[FLEXSPI_INTR / 4u] &= ~value; /* write 1 to clear */
            fspi.regs[FLEXSPI_INTR / 4u] |= INTR_IPCMDDONE;
            return MM_TRUE;
        case FLEXSPI_STS0:
            return MM_TRUE;
        default:
            break;
        }
    }
    memcpy((mm_u8 *)fspi.regs + offset, &value, size_bytes);
    return MM_TRUE;
}

mm_bool mm_rw612_flexspi_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    memset(&reg, 0, sizeof(reg));
    reg.size = FLEXSPI_SIZE;
    reg.read = flexspi_read;
    reg.write = flexspi_write;
    reg.base = FLEXSPI_BASE_NS;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = FLEXSPI_BASE_NS | 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    return MM_TRUE;
}

void mm_rw612_flexspi_reset(void)
{
    memset(fspi.regs, 0, sizeof(fspi.regs));
    fspi.regs[FLEXSPI_INTR / 4u] = INTR_IPCMDDONE;
    fspi.regs[FLEXSPI_STS0 / 4u] = STS0_SEQIDLE | STS0_ARBIDLE;
    fspi.store_warned = MM_FALSE;
}

static mm_bool xip_store(void *opaque, enum mm_sec_state sec, mm_u32 addr, mm_u32 size, mm_u32 value)
{
    (void)opaque;
    (void)sec;
    (void)size;
    (void)value;
    if (!fspi.store_warned) {
        fspi.store_warned = MM_TRUE;
        fprintf(stderr, "[RW612] CPU store to the FlexSPI XIP window at 0x%08lx: "
                "the NOR is programmed through the ROM FlexSPI NOR driver\n",
                (unsigned long)addr);
    }
    return MM_FALSE;
}

void mm_rw612_flexspi_bind(struct mm_memmap *map,
                           mm_u8 *flash,
                           mm_u32 flash_size,
                           const struct mm_flash_persist *persist)
{
    fspi.map = map;
    fspi.flash = flash;
    fspi.flash_size = flash_size;
    fspi.persist = persist;
    if (map != 0) {
        mm_memmap_set_flash_writer(map, xip_store, 0);
    }
}

mm_u32 mm_rw612_nor_size(void)
{
    return (fspi.flash != 0) ? fspi.flash_size : 0u;
}

static mm_bool nor_range_ok(mm_u32 off, mm_u32 len)
{
    if (fspi.flash == 0 || off >= fspi.flash_size) return MM_FALSE;
    return (len <= fspi.flash_size - off) ? MM_TRUE : MM_FALSE;
}

static void nor_changed(mm_u32 off, mm_u32 len)
{
    if (fspi.map != 0 && fspi.map->code_cache != 0) {
        mm_code_cache_note_write(fspi.map->code_cache, RW612_FLASH_BASE_NS + off, len);
        mm_code_cache_note_write(fspi.map->code_cache, RW612_FLASH_BASE_S + off, len);
    }
    if (fspi.persist != 0) {
        mm_flash_persist_flush((struct mm_flash_persist *)fspi.persist, off, len);
    }
}

mm_bool mm_rw612_nor_program(mm_u32 off, const mm_u8 *data, mm_u32 len)
{
    mm_u32 i;
    if (data == 0 || !nor_range_ok(off, len)) return MM_FALSE;
    if (trace_enabled()) {
        printf("[FLEXSPI] program off=0x%08lx len=%lu\n", (unsigned long)off, (unsigned long)len);
    }
    for (i = 0; i < len; ++i) {
        fspi.flash[off + i] &= data[i];
    }
    nor_changed(off, len);
    return MM_TRUE;
}

mm_bool mm_rw612_nor_erase(mm_u32 off, mm_u32 len)
{
    if (!nor_range_ok(off, len)) return MM_FALSE;
    if (trace_enabled()) {
        printf("[FLEXSPI] erase off=0x%08lx len=0x%lx\n", (unsigned long)off, (unsigned long)len);
    }
    memset(fspi.flash + off, 0xFF, len);
    nor_changed(off, len);
    return MM_TRUE;
}

mm_bool mm_rw612_nor_read(mm_u32 off, mm_u8 *out, mm_u32 len)
{
    if (out == 0 || !nor_range_ok(off, len)) return MM_FALSE;
    memcpy(out, fspi.flash + off, len);
    return MM_TRUE;
}
