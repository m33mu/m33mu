/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef M33MU_CPU_PSC3M6_CONFIG_H
#define M33MU_CPU_PSC3M6_CONFIG_H

#include "m33mu/target.h"

/* Infineon PSOC Control C3M6 (PSC3M6GES3AHQ1, CAT1B, Cortex-M33).
 *
 * Every memory has four views: Secure/Non-secure crossed with the code bus
 * (CBUS) and the system bus (SBUS).  Code executes from the CBUS view; the
 * boot ROM flash API takes SBUS addresses.  Bases follow the PDL device
 * headers (mtb-pdl-cat1 devices/COMPONENT_CAT1B/include/psc3m5*.h); the
 * sizes are the M6 ones (512 KB flash, 128 KB SRAM). */
#define PSC3M6_FLASH_BASE_S     0x12000000u   /* S CBUS */
#define PSC3M6_FLASH_BASE_NS    0x02000000u   /* NS CBUS */
#define PSC3M6_FLASH_BASE_S2    0x32000000u   /* S SBUS (flash API view) */
#define PSC3M6_FLASH_BASE_NS2   0x22000000u   /* NS SBUS */
#define PSC3M6_FLASH_SIZE       0x00080000u

#define PSC3M6_RAM_BASE_S       0x34000000u   /* S SBUS */
#define PSC3M6_RAM_BASE_NS      0x24000000u   /* NS SBUS */
#define PSC3M6_RAM_CBUS_S       0x14000000u
#define PSC3M6_RAM_CBUS_NS      0x04000000u
#define PSC3M6_RAM_SIZE         0x00020000u

#define PSC3M6_ROM_BASE_S       0x10800000u
#define PSC3M6_ROM_BASE_NS      0x00800000u
#define PSC3M6_ROM_SIZE         0x00010000u

#define PSC3M6_PERIPH_BASE_S    0x52000000u
#define PSC3M6_PERIPH_BASE_NS   0x42000000u

static const struct mm_ram_region PSC3M6_RAM_REGIONS[] = {
    { PSC3M6_RAM_BASE_S, PSC3M6_RAM_BASE_NS, PSC3M6_RAM_SIZE, -1, 0u },
    { PSC3M6_RAM_CBUS_S, PSC3M6_RAM_CBUS_NS, PSC3M6_RAM_SIZE, -1, 1u }
};

#define PSC3M6_RAM_REGION_COUNT (sizeof(PSC3M6_RAM_REGIONS) / sizeof(PSC3M6_RAM_REGIONS[0]))
#define PSC3M6_MPCBB_BLOCK_SIZE 0u

#define PSC3M6_SOC_RESET      mm_psc3m6_mmio_reset
#define PSC3M6_SOC_REGISTER   mm_psc3m6_register_mmio
#define PSC3M6_FLASH_BIND     mm_psc3m6_flash_bind
#define PSC3M6_CLOCK_GET_HZ   mm_psc3m6_cpu_hz
#define PSC3M6_USART_INIT     mm_psc3m6_scb_init
#define PSC3M6_USART_RESET    mm_psc3m6_scb_reset
#define PSC3M6_USART_POLL     mm_psc3m6_scb_poll

/* The boot ROM starts the application Secure; wolfBoot's TZEN=0 image links
 * at the Secure aliases and must not be demoted by --no-tz. */
#define PSC3M6_FLAGS (MM_TARGET_FLAG_FPU | MM_TARGET_FLAG_SECURE_BOOT)

#endif /* M33MU_CPU_PSC3M6_CONFIG_H */
