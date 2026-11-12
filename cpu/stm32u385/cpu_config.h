/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2026  Daniele Lacamera <root@danielinux.net>
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

#ifndef M33MU_CPU_STM32U385_CONFIG_H
#define M33MU_CPU_STM32U385_CONFIG_H

/* STM32U385RG memory map (RM0487): 1 MB dual-bank flash, 256 KB SRAM,
 * Secure / Non-secure aliases. */
#include "m33mu/target.h"

#define STM32U385_FLASH_BASE_S   0x0C000000u
#define STM32U385_FLASH_BASE_NS  0x08000000u
#define STM32U385_FLASH_SIZE     0x00100000u  /* 1 MB, 2 x 512 KB */

#define STM32U385_RAM_BASE_S     0x30000000u
#define STM32U385_RAM_BASE_NS    0x20000000u
#define STM32U385_RAM_SIZE       0x00040000u  /* 256 KB */

static const struct mm_ram_region STM32U385_RAM_REGIONS[] = {
    { 0x30000000u, 0x20000000u, 0x00030000u, -1, 0u }, /* SRAM1 192 KB */
    { 0x30030000u, 0x20030000u, 0x00010000u, -1, 0u }  /* SRAM2 64 KB */
};

#define STM32U385_RAM_REGION_COUNT (sizeof(STM32U385_RAM_REGIONS) / sizeof(STM32U385_RAM_REGIONS[0]))

#define STM32U385_SOC_RESET      mm_stm32u385_mmio_reset
#define STM32U385_SOC_REGISTER   mm_stm32u385_register_mmio
#define STM32U385_FLASH_BIND     mm_stm32u385_flash_bind
#define STM32U385_CLOCK_GET_HZ   mm_stm32u385_cpu_hz
#define STM32U385_USART_INIT     mm_stm32u385_usart_init
#define STM32U385_USART_RESET    mm_stm32u385_usart_reset
#define STM32U385_USART_POLL     mm_stm32u385_usart_poll

#define STM32U385_FLAGS (MM_TARGET_FLAG_NVM_WRITEONCE | MM_TARGET_FLAG_FPU)

#endif /* M33MU_CPU_STM32U385_CONFIG_H */
