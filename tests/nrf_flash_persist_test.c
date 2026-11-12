/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
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

/* Flash written through the nRF secure alias (0x10000000) must reach the
 * --persist file at its flash offset, as writes through the non-secure
 * alias at 0 do. */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/target.h"
#include "nrf5340/nrf5340_mmio.h"
#include "nrf54lm20/nrf54lm20_mmio.h"

#define FLASH_SIZE   0x4000u
#define BASE_S       0x10000000u
#define BASE_NS      0x00000000u

#define NVMC_BASE_S    0x50039000u
#define NVMC_CONFIG    0x504u
#define NVMC_ERASEPAGE 0x508u
#define RRAMC_BASE_S   0x5004E000u
#define RRAMC_CONFIG   0x500u

static mm_u8 flash[FLASH_SIZE];

static int open_persist(struct mm_flash_persist *persist, char *path)
{
    const char *paths[1];
    mm_u32 offsets[1];
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    close(fd);
    paths[0] = path;
    offsets[0] = 0u;
    memset(persist, 0, sizeof(*persist));
    mm_flash_persist_build(persist, flash, FLASH_SIZE, paths, offsets, 1);
    return persist->enabled ? 0 : -1;
}

static int file_byte(const char *path, long off, mm_u8 *out)
{
    FILE *f = fopen(path, "rb");
    int c;
    if (f == 0) return -1;
    if (fseek(f, off, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    c = fgetc(f);
    fclose(f);
    if (c == EOF) return -1;
    *out = (mm_u8)c;
    return 0;
}

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
}

static int test_nrf5340(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    struct mm_flash_persist persist;
    char path[] = "/tmp/m33mu_nrf5340_persist_XXXXXX";
    mm_u8 b;
    int rc = 1;

    memset(flash, 0xFF, sizeof(flash));
    if (open_persist(&persist, path) != 0) return 1;
    init_map(&map, regions, 64u);
    mm_nrf5340_mmio_reset();
    (void)mm_nrf5340_register_mmio(&map.mmio);
    mm_nrf5340_flash_bind(&map, flash, FLASH_SIZE, &persist, 0u);
    mmio_set_active_sec(MM_SECURE);

    /* program through the secure alias */
    if (!mmio_bus_write(&map.mmio, NVMC_BASE_S + NVMC_CONFIG, 4u, 1u)) goto out;
    if (!mm_memmap_write(&map, MM_SECURE, BASE_S + 0x1010u, 4u, 0x12345678u)) goto out;
    if (file_byte(path, 0x1010, &b) != 0 || b != 0x78) {
        fprintf(stderr, "nrf5340: secure-alias program not persisted\n");
        goto out;
    }
    /* erase the page through the secure alias */
    if (!mmio_bus_write(&map.mmio, NVMC_BASE_S + NVMC_CONFIG, 4u, 2u)) goto out;
    if (!mmio_bus_write(&map.mmio, NVMC_BASE_S + NVMC_ERASEPAGE, 4u, BASE_S + 0x1000u)) goto out;
    if (file_byte(path, 0x1010, &b) != 0 || b != 0xFF) {
        fprintf(stderr, "nrf5340: secure-alias page erase not persisted\n");
        goto out;
    }
    rc = 0;
out:
    mm_flash_persist_build(&persist, 0, 0, 0, 0, 0);
    unlink(path);
    return rc;
}

static int test_nrf54lm20(void)
{
    struct mm_memmap map;
    struct mmio_region regions[64];
    struct mm_flash_persist persist;
    char path[] = "/tmp/m33mu_nrf54_persist_XXXXXX";
    mm_u8 b;
    int rc = 1;

    memset(flash, 0xFF, sizeof(flash));
    if (open_persist(&persist, path) != 0) return 1;
    init_map(&map, regions, 64u);
    mm_nrf54lm20_mmio_reset();
    (void)mm_nrf54lm20_register_mmio(&map.mmio);
    mm_nrf54lm20_flash_bind(&map, flash, FLASH_SIZE, &persist, 0u);
    mmio_set_active_sec(MM_SECURE);

    if (!mmio_bus_write(&map.mmio, RRAMC_BASE_S + RRAMC_CONFIG, 4u, 1u)) goto out;
    /* nRF54L has no secure flash alias: the secure world programs RRAM at 0. */
    if (!mm_memmap_write(&map, MM_SECURE, BASE_NS + 0x2020u, 4u, 0x00AB00CDu)) goto out;
    if (file_byte(path, 0x2020, &b) != 0 || b != 0xCD) {
        fprintf(stderr, "nrf54lm20: secure program not persisted\n");
        goto out;
    }
    rc = 0;
out:
    mm_flash_persist_build(&persist, 0, 0, 0, 0, 0);
    unlink(path);
    return rc;
}

int main(void)
{
    int fails = 0;
    fails += test_nrf5340();
    fails += test_nrf54lm20();
    if (fails != 0) {
        printf("nrf_flash_persist_test: FAIL (%d)\n", fails);
        return 1;
    }
    printf("nrf_flash_persist_test: PASS\n");
    return 0;
}
