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

/* A core without an MPU (MCX A153) reports MPU_TYPE 0 and ignores the MPU
 * registers, so firmware that enables the MPU unconditionally keeps running. */

#include <stdio.h>
#include <string.h>
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/scs.h"
#include "m33mu/nvic.h"
#include "m33mu/mpu.h"

#define SCS_S   0xE000ED00u
#define MPU_TYPE (SCS_S + 0x90u)
#define MPU_CTRL (SCS_S + 0x94u)
#define MPU_RNR  (SCS_S + 0x98u)
#define MPU_RBAR (SCS_S + 0x9Cu)
#define MPU_RLAR (SCS_S + 0xA0u)

static int test_mpu(mm_bool present)
{
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_nvic nvic;
    struct mmio_region regions[8];
    mm_u32 v = 0;

    mm_memmap_init(&map, regions, 8u);
    mm_nvic_init(&nvic);
    mm_scs_init(&scs, 0u);
    mm_scs_set_mpu_present(&scs, present);
    if (!mm_scs_register_regions(&scs, &map.mmio, SCS_S, 0xE002ED00u, &nvic)) return 1;
    mmio_set_active_sec(MM_SECURE);

    if (!mmio_bus_read(&map.mmio, MPU_TYPE, 4u, &v)) return 1;
    if (present && v == 0u) {
        fprintf(stderr, "mpu: MPU_TYPE reads 0 with an MPU\n");
        return 1;
    }
    if (!present && v != 0u) {
        fprintf(stderr, "mpu: MPU_TYPE 0x%08lx without an MPU\n", (unsigned long)v);
        return 1;
    }
    if (!mmio_bus_write(&map.mmio, MPU_RNR, 4u, 0u)) return 1;
    if (!mmio_bus_write(&map.mmio, MPU_RBAR, 4u, 0x20000000u)) return 1;
    if (!mmio_bus_write(&map.mmio, MPU_RLAR, 4u, 0x2000FFE1u)) return 1;
    if (!mmio_bus_write(&map.mmio, MPU_CTRL, 4u, 1u)) return 1;
    if (!mmio_bus_read(&map.mmio, MPU_CTRL, 4u, &v)) return 1;
    if (present && (v != 1u || !mm_mpu_enabled(&scs, MM_SECURE))) {
        fprintf(stderr, "mpu: enable lost with an MPU\n");
        return 1;
    }
    if (!present && (v != 0u || mm_mpu_enabled(&scs, MM_SECURE))) {
        fprintf(stderr, "mpu: enabled without an MPU\n");
        return 1;
    }
    if (!mmio_bus_read(&map.mmio, MPU_RLAR, 4u, &v)) return 1;
    if (!present && v != 0u) {
        fprintf(stderr, "mpu: RLAR kept without an MPU\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;
    fails += test_mpu(MM_TRUE);
    fails += test_mpu(MM_FALSE);
    if (fails != 0) {
        printf("scs_mpu_present_test: FAIL (%d)\n", fails);
        return 1;
    }
    printf("scs_mpu_present_test: PASS\n");
    return 0;
}
