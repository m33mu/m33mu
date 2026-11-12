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

/* STM32WBA55 bring-up as wolfBoot's wolfHAL board does it: Range 1, HSE32
 * -> PLL1 -> 100 MHz SYSCLK, USART1 on PB12/PA8, then the non-secure flash
 * controller: page erase, 128-bit programming, the write-once rule and the
 * lock.  Run with --no-tz, as the TZEN=0 wolfBoot build is. */

#include <stdint.h>
#include <stdio.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern void __libc_init_array(void);

volatile uint32_t systick_ms = 0;

#define REG(a) (*(volatile uint32_t *)(a))

#define RCC_BASE        0x46020C00u
#define RCC_CR          REG(RCC_BASE + 0x000u)
#define RCC_CFGR1       REG(RCC_BASE + 0x01Cu)
#define RCC_PLL1CFGR    REG(RCC_BASE + 0x028u)
#define RCC_PLL1DIVR    REG(RCC_BASE + 0x034u)
#define RCC_AHB1ENR     REG(RCC_BASE + 0x088u)
#define RCC_AHB2ENR     REG(RCC_BASE + 0x08Cu)
#define RCC_AHB4ENR     REG(RCC_BASE + 0x094u)
#define RCC_APB2ENR     REG(RCC_BASE + 0x0A4u)
#define RCC_CFGR4       REG(RCC_BASE + 0x200u)

#define PWR_BASE        0x46020800u
#define PWR_VOSR        REG(PWR_BASE + 0x00Cu)

#define FLASH_BASE      0x40022000u
#define FLASH_ACR       REG(FLASH_BASE + 0x000u)
#define FLASH_NSKEYR    REG(FLASH_BASE + 0x008u)
#define FLASH_NSSR      REG(FLASH_BASE + 0x020u)
#define FLASH_NSCR1     REG(FLASH_BASE + 0x028u)

#define FLASH_KEY1      0x45670123u
#define FLASH_KEY2      0xCDEF89ABu
#define SR_EOP          (1u << 0)
#define SR_PROGERR      (1u << 3)
#define SR_PGSERR       (1u << 7)
#define SR_BSY          (1u << 16)
#define SR_WDW          (1u << 17)
#define SR_ERRORS       0x20FAu
#define CR_PG           (1u << 0)
#define CR_PER          (1u << 1)
#define CR_PNB_SHIFT    3
#define CR_STRT         (1u << 16)
#define CR_LOCK         (1u << 31)

#define TEST_PAGE       127u
#define TEST_ADDR       (0x08000000u + TEST_PAGE * 0x2000u)

#define GPIOA_BASE      0x42020000u
#define GPIOB_BASE      0x42020400u
#define GPIO_MODER(b)   REG((b) + 0x00u)
#define GPIO_AFRH(b)    REG((b) + 0x24u)

#define USART1_BASE     0x40013800u
#define USART_CR1       REG(USART1_BASE + 0x00u)
#define USART_BRR       REG(USART1_BASE + 0x0Cu)
#define USART_ISR       REG(USART1_BASE + 0x1Cu)
#define USART_TDR       REG(USART1_BASE + 0x28u)
#define ISR_TC          (1u << 6)

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

void console_putc(char c)
{
    USART_TDR = (uint32_t)c;
    while ((USART_ISR & ISR_TC) == 0u) {
    }
}

static void console_init(void)
{
    uint32_t v;
    RCC_AHB2ENR |= (1u << 0) | (1u << 1);
    RCC_APB2ENR |= (1u << 14);
    v = GPIO_MODER(GPIOB_BASE) & ~(3u << (12u * 2u));
    GPIO_MODER(GPIOB_BASE) = v | (2u << (12u * 2u));
    v = GPIO_AFRH(GPIOB_BASE) & ~(0xFu << 16);
    GPIO_AFRH(GPIOB_BASE) = v | (7u << 16);
    v = GPIO_MODER(GPIOA_BASE) & ~(3u << (8u * 2u));
    GPIO_MODER(GPIOA_BASE) = v | (2u << (8u * 2u));
    v = GPIO_AFRH(GPIOA_BASE) & ~0xFu;
    GPIO_AFRH(GPIOA_BASE) = v | 7u;
    USART_BRR = 100000000u / 115200u;
    USART_CR1 = (1u << 0) | (1u << 2) | (1u << 3) | (1u << 29);
}

/* wolfHAL_board.c pll_clock_on(): Range 1, 3 WS, HSE32 -> PLL1 (M=1, N=24,
 * R=3) -> SYSCLK, AHB5 /4 */
static void clock_100mhz(void)
{
    RCC_AHB4ENR |= (1u << 2);
    PWR_VOSR |= (1u << 16);
    while ((PWR_VOSR & (1u << 15)) == 0u) {
    }
    RCC_AHB1ENR |= (1u << 8);
    FLASH_ACR = (FLASH_ACR & ~0xFu) | 3u;
    RCC_CFGR4 = (RCC_CFGR4 & ~0x7u) | 5u;
    RCC_CR |= (1u << 16);
    while ((RCC_CR & (1u << 17)) == 0u) {
    }
    RCC_CR &= ~(1u << 24);
    while ((RCC_CR & (1u << 25)) != 0u) {
    }
    RCC_PLL1CFGR = 3u | (3u << 2) | (1u << 8) | (1u << 18);
    RCC_PLL1DIVR = 24u | (3u << 24);
    RCC_CR |= (1u << 24);
    while ((RCC_CR & (1u << 25)) == 0u) {
    }
    RCC_CFGR1 = (RCC_CFGR1 & ~3u) | 3u;
    while (((RCC_CFGR1 >> 2) & 3u) != 3u) {
    }
}

static void flash_wait(void)
{
    while ((FLASH_NSSR & SR_BSY) != 0u) {
    }
}

static void flash_unlock(void)
{
    if ((FLASH_NSCR1 & CR_LOCK) != 0u) {
        FLASH_NSKEYR = FLASH_KEY1;
        FLASH_NSKEYR = FLASH_KEY2;
    }
}

static void flash_erase_page(uint32_t page)
{
    flash_wait();
    FLASH_NSSR = SR_ERRORS;
    FLASH_NSCR1 = (FLASH_NSCR1 & ~(0x7Fu << CR_PNB_SHIFT)) | CR_PER |
                  (page << CR_PNB_SHIFT) | CR_STRT;
    flash_wait();
    FLASH_NSCR1 &= ~CR_PER;
}

/* stm32wba_flash.c whal_Stm32wba_Flash_Write(): one 128-bit flash word */
static void flash_program(uint32_t addr, const uint32_t *w)
{
    volatile uint32_t *dst = (volatile uint32_t *)addr;
    flash_wait();
    FLASH_NSCR1 |= CR_PG;
    dst[0] = w[0];
    dst[1] = w[1];
    dst[2] = w[2];
    dst[3] = w[3];
    flash_wait();
    FLASH_NSCR1 &= ~CR_PG;
}

static void flash_test(void)
{
    const volatile uint32_t *p = (const volatile uint32_t *)TEST_ADDR;
    static const uint32_t data[4] = { 0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u };
    static const uint32_t again[4] = { 0u, 0u, 0u, 0xFFFFFFFEu };
    uint32_t sr;

    check((FLASH_NSCR1 & CR_LOCK) != 0u, "flash locked after reset");
    flash_unlock();
    check((FLASH_NSCR1 & CR_LOCK) == 0u, "NSKEYR sequence unlocks NSCR1");

    flash_erase_page(TEST_PAGE);
    check((FLASH_NSSR & SR_EOP) != 0u, "page erase sets EOP");
    FLASH_NSSR = SR_EOP;
    check((FLASH_NSSR & SR_EOP) == 0u, "EOP clears on write 1");
    check(p[0] == 0xFFFFFFFFu && p[3] == 0xFFFFFFFFu, "erased page reads 0xFF");

    flash_program(TEST_ADDR, data);
    check((FLASH_NSSR & SR_WDW) == 0u, "WDW clear once the flash word is in");
    check((FLASH_NSSR & SR_ERRORS) == 0u, "program raises no error");
    check(p[0] == data[0] && p[1] == data[1] && p[2] == data[2] && p[3] == data[3],
          "flash word reads back");

    flash_program(TEST_ADDR, again);
    sr = FLASH_NSSR;
    check((sr & SR_PROGERR) != 0u, "second program of a flash word is PROGERR");
    check(p[0] == data[0] && p[3] == data[3], "refused program leaves the word alone");
    FLASH_NSSR = SR_ERRORS;
    check((FLASH_NSSR & SR_ERRORS) == 0u, "error flags clear on write 1");

    FLASH_NSCR1 |= CR_PG;
    *(volatile uint32_t *)(TEST_ADDR + 16u) = 0x12345678u;
    check((FLASH_NSSR & SR_WDW) != 0u, "partial flash word sets WDW");
    *(volatile uint32_t *)(TEST_ADDR + 48u) = 0x12345678u;
    check((FLASH_NSSR & SR_PGSERR) != 0u, "store to another word with WDW is PGSERR");
    check(p[4] == 0xFFFFFFFFu && p[12] == 0xFFFFFFFFu, "both stores are lost");
    FLASH_NSSR = SR_ERRORS;
    FLASH_NSCR1 &= ~CR_PG;

    FLASH_NSCR1 |= CR_LOCK;
    check((FLASH_NSCR1 & CR_LOCK) != 0u, "LOCK set by software");
    FLASH_NSCR1 |= CR_PG;
    check((FLASH_NSCR1 & CR_PG) == 0u, "NSCR1 ignores writes while locked");
    *(volatile uint32_t *)(TEST_ADDR + 32u) = 0u;
    check(p[8] == 0xFFFFFFFFu, "store while locked is ignored");
    FLASH_NSSR = SR_ERRORS;
}

int main(void)
{
    clock_100mhz();
    console_init();
    printf("STM32WBA55 test start\n");
    check(((RCC_CFGR1 >> 2) & 3u) == 3u, "SYSCLK from PLL1");
    flash_test();
    if (failures == 0) {
        printf("ALL PASS\n");
        __asm volatile("bkpt #0x7f");
    } else {
        printf("FAILURES: %d\n", failures);
        __asm volatile("bkpt #0x7e");
    }
    while (1) {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    uint32_t *src;
    uint32_t *dst;

    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; ) {
        *dst++ = 0u;
    }
    __libc_init_array();
    main();
}

void HardFault_Handler(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) {
    }
}
