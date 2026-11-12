/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* nRF54L15: 1524 KB RRAM, 256 KB RAM, GPIO P0..P2. Register behaviour
 * lives in cpu/nrf54l_mmio.c; this file is the part description. */

#include "nrf54l15/nrf54l15_mmio.h"
#include "nrf54l_mmio.h"

/* DPPIC, GPIOTE[0..1], GPIO[n].PIN[0..31] and GRTC features (MDK SVD). */
static const struct nrf54l_spu_feature g_spu_features[] = {
    { 0x680u, 32u },
    { 0x700u, 32u },
    { 0x800u, 96u },
    { 0xD00u, 24u },
    { 0xD74u, 3u },
    { 0xD80u, 16u }
};

static const struct nrf54l_mmio_variant g_variant = {
    0x5004B000u, /* rramc_base */
    0x50048000u, /* cracen_base */
    0x51800000u, /* cracencore_base */
    0x500DC000u, /* tampc_base */
    3,           /* gpio_count: P0..P2 */
    7u,          /* mpc_override_count */
    5u,          /* mpc_block_override */
    0x00040000u, /* ram_size: 256 KB */
    0x100u,      /* ficr_info_ram: 256 KB */
    0x5F4u,      /* ficr_info_rram: 1524 KB */
    261u,        /* clock_irq: CLOCK_POWER */
    g_spu_features,
    sizeof(g_spu_features) / sizeof(g_spu_features[0])
};

mm_bool mm_nrf54l15_register_mmio(struct mmio_bus *bus)
{
    return nrf54l_register_mmio(&g_variant, bus);
}

void mm_nrf54l15_flash_bind(struct mm_memmap *map,
                            mm_u8 *flash,
                            mm_u32 flash_size,
                            const struct mm_flash_persist *persist,
                            mm_u32 flags)
{
    nrf54l_flash_bind(&g_variant, map, flash, flash_size, persist, flags);
}

mm_u64 mm_nrf54l15_cpu_hz(void)
{
    return nrf54l_cpu_hz();
}

void mm_nrf54l15_mmio_reset(void)
{
    nrf54l_mmio_reset(&g_variant);
}

mm_bool mm_nrf54l15_tz_attr_for_addr(mm_u32 addr,
                                     enum mm_sau_attr *attr_out,
                                     mm_u32 *region_out)
{
    return nrf54l_tz_attr_for_addr(addr, attr_out, region_out);
}
