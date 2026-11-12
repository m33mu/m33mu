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

#include "stm32wba55/stm32wba55_usart.h"
#include "stm32wba55/stm32wba55_mmio.h"
#include "stm32_usart.h"

static struct stm32_usart_state g_usart;

/* RCC_APB2ENR.USART1EN (RM0493) */
static mm_bool clock_apb2_usart1(struct stm32_usart_inst *u)
{
    if (u->rcc_regs == 0) return MM_TRUE;
    return ((u->rcc_regs[0xa4 / 4] >> 14) & 1u) != 0u;
}

/* RCC_APB1ENR1.USART2EN */
static mm_bool clock_apb1enr1_usart2(struct stm32_usart_inst *u)
{
    if (u->rcc_regs == 0) return MM_TRUE;
    return ((u->rcc_regs[0x9c / 4] >> 17) & 1u) != 0u;
}

/* RCC_APB7ENR.LPUART1EN */
static mm_bool clock_apb7_lpuart1(struct stm32_usart_inst *u)
{
    if (u->rcc_regs == 0) return MM_TRUE;
    return ((u->rcc_regs[0xa8 / 4] >> 6) & 1u) != 0u;
}

void mm_stm32wba55_usart_poll(void)
{
    stm32_usart_poll(&g_usart);
}

void mm_stm32wba55_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    static const mm_u32 bases[] = { 0x40013800u, 0x40004400u, 0x46002400u };
    static const int irq_map[] = { 46, 47, 48 };
    static const char *labels[] = { "USART1", "USART2", "LPUART1" };
    size_t i;
    stm32_usart_state_init(&g_usart, sizeof(bases) / sizeof(bases[0]), nvic);
    for (i = 0; i < g_usart.usart_count; ++i) {
        struct stm32_usart_inst *u;
        stm32_usart_register_instance(&g_usart, bus, i, bases[i], irq_map[i], labels[i],
                                      MM_FALSE, MM_TRUE, MM_FALSE);
        u = &g_usart.usarts[i];
        u->rcc_regs = mm_stm32wba55_rcc_regs();
        if (i == 0) {
            u->clock_on = clock_apb2_usart1;
        } else if (i == 1) {
            u->clock_on = clock_apb1enr1_usart2;
        } else {
            u->clock_on = clock_apb7_lpuart1;
        }
    }
}

void mm_stm32wba55_usart_reset(void)
{
    stm32_usart_reset(&g_usart);
}
