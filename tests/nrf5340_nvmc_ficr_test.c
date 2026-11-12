/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2026  wolfSSL Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

/* nRF5340 NVMC page erase (CONFIG.WEN=Een, write 0xFFFFFFFF to the page) and
 * the FICR block the wolfBoot HAL reads for the CryptoCell TRNG setup. */

#include <stdio.h>
#include <string.h>
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/target.h"
#include "nrf5340/nrf5340_mmio.h"

#define FLASH_SIZE   0x4000u
#define BASE_S       0x10000000u
#define BASE_NS      0x00000000u

#define NVMC_BASE_NS   0x40039000u
#define NVMC_BASE_S    0x50039000u
#define NVMC_CONFIG    0x504u
#define NVMC_CONFIGNS  0x584u
#define CONFIG_WEN     1u
#define CONFIG_EEN     2u

#define FICR_BASE      0x00FF0000u

static mm_u8 flash[FLASH_SIZE];

static void init_map(struct mm_memmap *map, struct mmio_region *regions, size_t cap)
{
    struct mm_target_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = BASE_S;
    cfg.flash_size_s = FLASH_SIZE;
    cfg.flash_base_ns = BASE_NS;
    cfg.flash_size_ns = FLASH_SIZE;
    mm_memmap_init(map, regions, cap);
    (void)mm_memmap_configure_flash(map, &cfg, flash, MM_TRUE);
    map->flash_base_s = BASE_S;
    map->flash_size_s = FLASH_SIZE;
    map->flash_base_ns = BASE_NS;
    map->flash_size_ns = FLASH_SIZE;
    mm_nrf5340_mmio_reset();
    (void)mm_nrf5340_register_mmio(&map->mmio);
    mm_nrf5340_flash_bind(map, flash, FLASH_SIZE, 0, 0u);
}

static int page_is(mm_u32 off, mm_u8 val)
{
    mm_u32 i;
    for (i = 0; i < 0x1000u; ++i) {
        if (flash[off + i] != val) return 0;
    }
    return 1;
}

static int test_een_page_erase(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];

    memset(flash, 0x00, sizeof(flash));
    init_map(&map, regions, 64u);
    mmio_set_active_sec(MM_SECURE);

    /* secure alias, page 1, as the wolfBoot HAL does */
    if (!mmio_bus_write(&map.mmio, NVMC_BASE_S + NVMC_CONFIG, 4u, CONFIG_EEN)) return 1;
    if (!mm_memmap_write(&map, MM_SECURE, BASE_S + 0x1000u, 4u, 0xFFFFFFFFu)) {
        fprintf(stderr, "een: secure page erase rejected\n");
        return 1;
    }
    if (!page_is(0x1000u, 0xFFu) || !page_is(0x0000u, 0x00u) || !page_is(0x2000u, 0x00u)) {
        fprintf(stderr, "een: secure page erase hit the wrong range\n");
        return 1;
    }
    /* any other value is not an erase */
    if (mm_memmap_write(&map, MM_SECURE, BASE_S + 0x2000u, 4u, 0x0u)) {
        fprintf(stderr, "een: non-erase write accepted\n");
        return 1;
    }
    /* only the first word of a page starts an erase */
    if (mm_memmap_write(&map, MM_SECURE, BASE_S + 0x2010u, 4u, 0xFFFFFFFFu)) {
        fprintf(stderr, "een: interior-word write erased\n");
        return 1;
    }
    /* CONFIG=Een covers only the Secure view: the Non-secure view stays Ren */
    if (mm_memmap_write(&map, MM_SECURE, BASE_NS + 0x3000u, 4u, 0xFFFFFFFFu)) {
        fprintf(stderr, "een: non-secure view erased under CONFIG\n");
        return 1;
    }
    /* CONFIG=Ren, CONFIGNS=Een: the Non-secure view is erasable by both
     * worlds (a secure bootloader updating the non-secure image) */
    if (!mmio_bus_write(&map.mmio, NVMC_BASE_S + NVMC_CONFIG, 4u, 0u)) return 1;
    if (!mmio_bus_write(&map.mmio, NVMC_BASE_S + NVMC_CONFIGNS, 4u, CONFIG_EEN)) return 1;
    if (!mm_memmap_write(&map, MM_SECURE, BASE_NS + 0x3000u, 4u, 0xFFFFFFFFu)) {
        fprintf(stderr, "een: secure erase of the non-secure view rejected\n");
        return 1;
    }
    if (!page_is(0x3000u, 0xFFu) || !page_is(0x2000u, 0x00u)) {
        fprintf(stderr, "een: non-secure view erase hit the wrong range\n");
        return 1;
    }
    mmio_set_active_sec(MM_NONSECURE);
    if (!mm_memmap_write(&map, MM_NONSECURE, BASE_NS + 0x2000u, 4u, 0xFFFFFFFFu) ||
        !page_is(0x2000u, 0xFFu)) {
        fprintf(stderr, "een: non-secure page erase rejected\n");
        return 1;
    }
    /* the Secure view is out of reach for Non-secure code, and CONFIG is Ren */
    flash[0x1000u] = 0x00u;
    if (mm_memmap_write(&map, MM_NONSECURE, BASE_S + 0x1000u, 4u, 0xFFFFFFFFu) ||
        mm_memmap_write(&map, MM_SECURE, BASE_S + 0x1000u, 4u, 0xFFFFFFFFu) ||
        flash[0x1000u] != 0x00u) {
        fprintf(stderr, "een: secure view erased without CONFIG=Een\n");
        return 1;
    }
    /* back to WEN: plain programming still clears bits only */
    if (!mmio_bus_write(&map.mmio, NVMC_BASE_NS + NVMC_CONFIGNS, 4u, CONFIG_WEN)) return 1;
    if (!mm_memmap_write(&map, MM_NONSECURE, BASE_NS + 0x3000u, 4u, 0x12345678u)) return 1;
    if (flash[0x3000u] != 0x78u || flash[0x3003u] != 0x12u) {
        fprintf(stderr, "wen: program after erase failed\n");
        return 1;
    }
    if (!mm_memmap_write(&map, MM_SECURE, BASE_NS + 0x2004u, 4u, 0xA5A5A5A5u) ||
        flash[0x2004u] != 0xA5u) {
        fprintf(stderr, "wen: secure program of the non-secure view failed\n");
        return 1;
    }
    return 0;
}

static int test_ficr(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    mm_u32 v = 0;

    init_map(&map, regions, 64u);
    mmio_set_active_sec(MM_SECURE);
    if (!mm_memmap_read(&map, MM_SECURE, FICR_BASE + 0x20Cu, 4u, &v) || v != 0x00005340u) {
        fprintf(stderr, "ficr: INFO.PART 0x%08lx\n", (unsigned long)v);
        return 1;
    }
    if (!mm_memmap_read(&map, MM_SECURE, FICR_BASE + 0xC10u, 4u, &v) ||
        v == 0u || v == 0xFFFFFFFFu) {
        fprintf(stderr, "ficr: TRNG90B.ROSC1 0x%08lx\n", (unsigned long)v);
        return 1;
    }
    if (!mm_memmap_read(&map, MM_SECURE, FICR_BASE + 0xC00u, 4u, &v) || v != 0x00000210u) {
        fprintf(stderr, "ficr: TRNG90B.BYTES 0x%08lx\n", (unsigned long)v);
        return 1;
    }
    if (!mm_memmap_read(&map, MM_SECURE, FICR_BASE + 0x220u, 4u, &v) || v != 0x00001000u ||
        !mm_memmap_read(&map, MM_SECURE, FICR_BASE + 0x224u, 4u, &v) || v != 0x00000100u) {
        fprintf(stderr, "ficr: INFO.CODEPAGESIZE/CODESIZE\n");
        return 1;
    }
    if (mm_memmap_write(&map, MM_SECURE, FICR_BASE + 0xC10u, 4u, 0u)) {
        fprintf(stderr, "ficr: write accepted\n");
        return 1;
    }
    /* FICR is Secure-only */
    mmio_set_active_sec(MM_NONSECURE);
    if (mm_memmap_read(&map, MM_NONSECURE, FICR_BASE + 0xC10u, 4u, &v)) {
        fprintf(stderr, "ficr: non-secure read accepted\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;
    fails += test_een_page_erase();
    fails += test_ficr();
    if (fails != 0) {
        printf("nrf5340_nvmc_ficr_test: FAIL (%d)\n", fails);
        return 1;
    }
    printf("nrf5340_nvmc_ficr_test: PASS\n");
    return 0;
}
