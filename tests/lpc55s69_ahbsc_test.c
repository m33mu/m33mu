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

/* LPC55S69 AHB secure controller: SRAM rules apply once either MISC_CTRL copy
 * enables secure checking; strict mode refuses Secure accesses to Non-secure
 * rules unless both copies select tier mode. */

#include <stdio.h>
#include <string.h>
#include "m33mu/memmap.h"
#include "m33mu/mem_prot.h"
#include "m33mu/mmio.h"
#include "m33mu/scs.h"
#include "m33mu/cpu_db.h"
#include "lpc55s69/lpc55s69_mmio.h"

#define AHBSC_SEC_BASE      0x500AC000u
#define AHBSC_RAM2_RULE0    0x0A0u
#define AHBSC_MISC_CTRL_DP  0xFF8u
#define AHBSC_MISC_CTRL     0xFFCu

static mm_u8 flash[0x100000];
static mm_u8 ram[0x44000];

static int fail(const char *what)
{
    printf("lpc55s69_ahbsc_test: FAIL (%s)\n", what);
    return 1;
}

static mm_bool all_secure_block(int bank, mm_u32 block_index)
{
    (void)bank;
    (void)block_index;
    return MM_TRUE;
}

/* The LPC55S69 gate (checking off at reset) must not reach another target. */
static int test_other_target_keeps_its_mpc(void)
{
    static mm_u8 other_ram[0x10000];
    static const struct mm_ram_region other_regions[] = {
        { 0x30000000u, 0x20000000u, sizeof(other_ram), 0, 0u }
    };
    struct mm_target_cfg other;
    struct mm_memmap omap;
    struct mmio_region oregions[4];
    struct mm_scs oscs;
    struct mm_prot_ctx oprot;

    memset(&other, 0, sizeof(other));
    other.ram_base_s = 0x30000000u;
    other.ram_size_s = sizeof(other_ram);
    other.ram_base_ns = 0x20000000u;
    other.ram_size_ns = sizeof(other_ram);
    other.ram_regions = other_regions;
    other.ram_region_count = 1u;
    other.mpcbb_block_size = 0x1000u;
    other.mpcbb_block_secure = all_secure_block;
    mm_memmap_init(&omap, oregions, 4u);
    if (!mm_memmap_configure_ram(&omap, &other, other_ram, MM_TRUE)) return 1;
    mm_scs_init(&oscs, 0);
    oscs.sau_ctrl = 0x1u;
    oscs.sau_rbar[0] = 0x20000000u;
    oscs.sau_rlar[0] = 0x2000FFE0u | 0x1u;
    mm_prot_init(&oprot, &oscs, &other, 0);
    mm_memmap_set_interceptor(&omap, mm_prot_interceptor, &oprot);
    mm_prot_add_region(&oprot, other.ram_base_ns, other.ram_size_ns,
        MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
    /* its own block map says Secure, so a Non-secure access must fault */
    return mm_memmap_write(&omap, MM_NONSECURE, 0x20000000u, 4u, 0u) ? 1 : 0;
}

int main(void)
{
    struct mm_target_cfg cfg;
    struct mm_memmap map;
    struct mmio_region regions[128];
    struct mm_scs scs;
    struct mm_prot_ctx prot;
    mm_u32 v = 0;

    if (!mm_cpu_lookup("lpc55s69", &cfg)) return fail("lookup");
    mm_memmap_init(&map, regions, 128u);
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE)) return 1;
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;
    mm_lpc55s69_mmio_reset();
    if (!mm_lpc55s69_register_mmio(&map.mmio)) return fail("register");

    /* wolfBoot TZ layout: NS app RAM 0x20020000..0x20027FFF */
    mm_scs_init(&scs, 0);
    scs.sau_ctrl = 0x1u;
    scs.sau_rbar[2] = 0x20020000u;
    scs.sau_rlar[2] = 0x20027FE0u | 0x1u;
    mm_prot_init(&prot, &scs, &cfg, 0);
    mm_memmap_set_interceptor(&map, mm_prot_interceptor, &prot);
    mm_prot_add_region(&prot, cfg.ram_base_s, cfg.ram_size_s,
        MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
    mm_prot_add_region(&prot, cfg.ram_base_ns, cfg.ram_size_ns,
        MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);

    mmio_set_active_sec(MM_SECURE);
    if (!mmio_bus_read(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL, 4u, &v) ||
        v != 0x0000AAAAu) return fail("MISC_CTRL_REG reset value");

    /* checking disabled at reset: the SAU alone decides */
    if (!mm_memmap_write(&map, MM_SECURE, 0x30020000u, 4u, 0x1u)) return fail("S write, check off");
    if (!mm_memmap_write(&map, MM_NONSECURE, 0x20020000u, 4u, 0x2u)) return fail("NS write, check off");
    if (mm_memmap_write(&map, MM_NONSECURE, 0x20000000u, 4u, 0x3u)) return fail("NS write to SAU-secure SRAM");
    scs.securefault_pending = MM_FALSE;

    /* checking stays on unless both MISC_CTRL copies disable it */
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL_DP, 4u, 0x0000AAA5u)) return 1;
    if (mm_memmap_write(&map, MM_NONSECURE, 0x20020000u, 4u, 0x4u)) return fail("NS write, checking on in DP only");
    scs.securefault_pending = MM_FALSE;

    /* enable checking as the NXP TZ init does: RAM2 rules are secure */
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL, 4u, 0x0000AAA6u)) return 1;
    if (mm_memmap_write(&map, MM_NONSECURE, 0x20020000u, 4u, 0x4u)) return fail("NS write to secure rule");
    scs.securefault_pending = MM_FALSE;
    if (!mm_memmap_write(&map, MM_SECURE, 0x30020000u, 4u, 0x5u)) return fail("S write to secure rule");

    /* rule 0 (non-secure user) for the first RAM2 block */
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_RAM2_RULE0, 4u, 0x33333330u)) return 1;
    if (!mm_memmap_write(&map, MM_NONSECURE, 0x20020000u, 4u, 0x6u)) return fail("NS write to NS rule");
    /* strict mode (reset): the Secure alias of a Non-secure rule faults */
    if (mm_memmap_write(&map, MM_SECURE, 0x30020000u, 4u, 0x7u)) return fail("S write to NS rule, strict");
    /* tier mode in one copy only is still strict */
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL, 4u, 0x00009AA6u)) return 1;
    if (mm_memmap_write(&map, MM_SECURE, 0x30020000u, 4u, 0x7u)) return fail("S write to NS rule, tier in REG only");
    /* smart-master tier mode in both copies: Secure may access the Non-secure rule */
    if (!mmio_bus_write(&map.mmio, AHBSC_SEC_BASE + AHBSC_MISC_CTRL_DP, 4u, 0x00009AA5u)) return 1;
    if (!mm_memmap_write(&map, MM_SECURE, 0x30020000u, 4u, 0x8u)) return fail("S write to NS rule, tier");
    if (!mm_memmap_read(&map, MM_SECURE, 0x30020000u, 4u, &v) || v != 0x8u) return fail("readback");

    if (test_other_target_keeps_its_mpc() != 0) return fail("gate leaked to another target");

    printf("lpc55s69_ahbsc_test: PASS\n");
    return 0;
}
