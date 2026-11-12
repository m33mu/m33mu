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

/* MCXN947 SRAM security comes from the AHB secure controller: the SAU alone
 * decides until MISC_CTRL enables secure checking, then RAMx_MEM_RULE applies
 * and strict mode refuses the Secure alias of a Non-secure rule. */

#include <stdio.h>
#include <string.h>
#include "m33mu/memmap.h"
#include "m33mu/mem_prot.h"
#include "m33mu/mmio.h"
#include "m33mu/scs.h"
#include "m33mu/cpu_db.h"
#include "mcxn947/mcxn947_mmio.h"

#define AHBSC_SEC_BASE          0x50120000u
#define AHBSC_RAMD_MEM_RULE0    0x100u
#define AHBSC_MISC_CTRL_DP_REG  0xFF8u
#define AHBSC_MISC_CTRL_REG     0xFFCu
#define AHBSC_MISC_CTRL_RESET   0x0000AAAAu
/* SDK tzm_config.c value: checking on, strict (tier) mode off */
#define AHBSC_MISC_CTRL_SDK     0x000086A6u
#define AHBSC_MISC_CTRL_STRICT  0x00008AA6u

#define TRDC_SEC_BASE           0x500C7000u
#define TRDC_MBC0_NSE_BLK_INDEX 0x10u
#define TRDC_MBC0_NSE_BLK_CLR   0x18u
#define TRDC_NSE_DID_SEL0       (1u << 16)

static mm_u8 flash[0x200000];
static mm_u8 ram[0x80000];

static int fail(const char *what)
{
    printf("mcxn947_sram_security_test: FAIL (%s)\n", what);
    return 1;
}

int main(void)
{
    struct mm_target_cfg cfg;
    struct mm_memmap map;
    struct mmio_region regions[128];
    struct mm_scs scs;
    struct mm_prot_ctx prot;
    mm_u32 v = 0;

    if (!mm_cpu_lookup("mcxn947", &cfg)) return fail("lookup");
    mm_memmap_init(&map, regions, 128u);
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE)) return 1;
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;
    mm_mcxn947_mmio_reset();
    if (!mm_mcxn947_register_mmio(&map.mmio)) return fail("register");

    /* wolfBoot TZ layout: NS app RAM window 0x20020000..0x20025FFF (RAMD) */
    mm_scs_init(&scs, 0);
    scs.sau_ctrl = 0x1u;
    scs.sau_rbar[2] = 0x20020000u;
    scs.sau_rlar[2] = 0x20025FE0u | 0x1u;
    mm_prot_init(&prot, &scs, &cfg, 0);
    mm_memmap_set_interceptor(&map, mm_prot_interceptor, &prot);
    mm_prot_add_region(&prot, cfg.ram_base_s, cfg.ram_size_s,
        MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
    mm_prot_add_region(&prot, cfg.ram_base_ns, cfg.ram_size_ns,
        MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
    mm_prot_add_region(&prot, 0x40000000u, 0x20000000u,
        MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);

    mmio_set_active_sec(MM_SECURE);
    if (!mmio_bus_read(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL_REG, 4u, &v) ||
        v != AHBSC_MISC_CTRL_RESET) return fail("MISC_CTRL_REG reset value");

    /* checking off at reset: a Secure image owns the secure alias, the SAU
     * alone gates Non-secure accesses */
    if (!mm_memmap_write(&map, MM_SECURE, 0x30000000u, 4u, 0x11223344u) ||
        !mm_memmap_read(&map, MM_SECURE, 0x30000000u, 4u, &v) || v != 0x11223344u)
        return fail("secure SRAM write at reset");
    if (!mm_memmap_write(&map, MM_SECURE, 0x30020000u, 4u, 0x1u)) return fail("S write to NS window, check off");
    if (!mm_memmap_write(&map, MM_NONSECURE, 0x20020000u, 4u, 0x55667788u)) return fail("NS write to SAU-NS SRAM");
    if (mm_memmap_write(&map, MM_NONSECURE, 0x20000000u, 4u, 0u)) return fail("NS write to SAU-secure SRAM");
    scs.securefault_pending = MM_FALSE;

    /* TRDC MBC0 no longer attributes the CPU view of SRAM */
    if (!mmio_bus_write(&map.mmio, TRDC_SEC_BASE + TRDC_MBC0_NSE_BLK_INDEX, 4u, TRDC_NSE_DID_SEL0)) return 1;
    if (!mmio_bus_write(&map.mmio, TRDC_SEC_BASE + TRDC_MBC0_NSE_BLK_CLR, 4u, 1u << 4)) return 1;
    if (!mm_memmap_write(&map, MM_NONSECURE, 0x20020000u, 4u, 0x2u)) return fail("TRDC changed SRAM attribution");

    /* enable checking as the SDK TZ init does: all rules reset to NS, so the
     * NS window stays reachable and the Secure alias is allowed (tier mode) */
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL_REG, 4u, AHBSC_MISC_CTRL_SDK)) return 1;
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL_DP_REG, 4u, AHBSC_MISC_CTRL_SDK)) return 1;
    if (!mm_memmap_write(&map, MM_NONSECURE, 0x20020000u, 4u, 0x3u)) return fail("NS write to NS rule");
    if (!mm_memmap_write(&map, MM_SECURE, 0x30020000u, 4u, 0x4u)) return fail("S write to NS rule, tier");

    /* both copies must leave strict mode; one copy asking for it is enough
     * to refuse the Secure alias of a Non-secure rule */
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL_REG, 4u, AHBSC_MISC_CTRL_STRICT)) return 1;
    if (mm_memmap_write(&map, MM_SECURE, 0x30020000u, 4u, 0x5u)) return fail("S write to NS rule, strict in REG only");
    scs.securefault_pending = MM_FALSE;
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL_DP_REG, 4u, AHBSC_MISC_CTRL_STRICT)) return 1;
    if (mm_memmap_write(&map, MM_SECURE, 0x30020000u, 4u, 0x6u)) return fail("S write to NS rule, strict");
    scs.securefault_pending = MM_FALSE;
    if (!mm_memmap_write(&map, MM_NONSECURE, 0x20020000u, 4u, 0x6u)) return fail("NS write to NS rule, strict");

    /* RAMD rule 0 (0x20020000, 4 KB) secure-privileged: NS refused, Secure ok;
     * the next 4 KB rule is untouched */
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_RAMD_MEM_RULE0, 4u, 0x00000003u)) return 1;
    if (mm_memmap_write(&map, MM_NONSECURE, 0x20020000u, 4u, 0x7u)) return fail("NS write to secure rule");
    scs.securefault_pending = MM_FALSE;
    if (!mm_memmap_write(&map, MM_SECURE, 0x30020000u, 4u, 0x8u)) return fail("S write to secure rule");
    if (!mm_memmap_read(&map, MM_SECURE, 0x30020000u, 4u, &v) || v != 0x8u) return fail("readback");
    if (!mm_memmap_write(&map, MM_NONSECURE, 0x20021000u, 4u, 0x9u)) return fail("NS write to next NS rule");

    /* disabling checking in both copies returns to SAU-only attribution */
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL_REG, 4u, AHBSC_MISC_CTRL_RESET)) return 1;
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL_DP_REG, 4u, AHBSC_MISC_CTRL_RESET)) return 1;
    if (!mm_memmap_write(&map, MM_NONSECURE, 0x20020000u, 4u, 0xAu)) return fail("NS write, checking off again");

    printf("mcxn947_sram_security_test: PASS\n");
    return 0;
}
