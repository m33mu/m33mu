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

/* STM32U385 bring-up as wolfBoot's hal/stm32u3.c does it: SMPS, EPOD
 * booster, Range 1, MSIS from MSIRC0 at 96 MHz, ICACHE, USART1 on PA9/PA10,
 * then the non-secure flash controller: bank 2 page erase, 64-bit
 * programming, the write-once rule and the lock.  Run with --no-tz, as the
 * TZEN=0 wolfBoot build is. */

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

#define RCC_BASE        0x40030C00u
#define RCC_CR          REG(RCC_BASE + 0x000u)
#define RCC_ICSCR1      REG(RCC_BASE + 0x008u)
#define RCC_CFGR1       REG(RCC_BASE + 0x01Cu)
#define RCC_CFGR2       REG(RCC_BASE + 0x020u)
#define RCC_CFGR4       REG(RCC_BASE + 0x028u)
#define RCC_AHB2ENR1    REG(RCC_BASE + 0x08Cu)
#define RCC_AHB1ENR2    REG(RCC_BASE + 0x094u)
#define RCC_APB2ENR     REG(RCC_BASE + 0x0A4u)

#define PWR_BASE        0x40030800u
#define PWR_CR3         REG(PWR_BASE + 0x008u)
#define PWR_VOSR        REG(PWR_BASE + 0x00Cu)
#define PWR_SVMSR       REG(PWR_BASE + 0x03Cu)

#define ICACHE_BASE     0x40030400u
#define ICACHE_CR       REG(ICACHE_BASE + 0x000u)
#define ICACHE_SR       REG(ICACHE_BASE + 0x004u)
#define ICACHE_FCR      REG(ICACHE_BASE + 0x00Cu)

#define FLASH_BASE      0x40022000u
#define FLASH_ACR       REG(FLASH_BASE + 0x000u)
#define FLASH_NSKEYR    REG(FLASH_BASE + 0x008u)
#define FLASH_NSSR      REG(FLASH_BASE + 0x020u)
#define FLASH_NSCR      REG(FLASH_BASE + 0x028u)

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
#define CR_BKER         (1u << 11)
#define CR_STRT         (1u << 16)
#define CR_LOCK         (1u << 31)

#define TEST_PAGE       127u
#define TEST_ADDR       (0x08080000u + TEST_PAGE * 0x1000u)

#define GPIOA_BASE      0x42020000u
#define GPIO_MODER(b)   REG((b) + 0x00u)
#define GPIO_AFRH(b)    REG((b) + 0x24u)

#define USART1_BASE     0x40013800u
#define USART_CR1       REG(USART1_BASE + 0x00u)
#define USART_BRR       REG(USART1_BASE + 0x0Cu)
#define USART_ISR       REG(USART1_BASE + 0x1Cu)
#define USART_TDR       REG(USART1_BASE + 0x28u)
#define ISR_TXE         (1u << 7)

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
    while ((USART_ISR & ISR_TXE) == 0u) {
    }
    USART_TDR = (uint32_t)c;
}

static void console_init(void)
{
    uint32_t v;
    RCC_AHB2ENR1 |= (1u << 0);
    v = GPIO_MODER(GPIOA_BASE) & ~((3u << 18) | (3u << 20));
    GPIO_MODER(GPIOA_BASE) = v | (2u << 18) | (2u << 20);
    v = GPIO_AFRH(GPIOA_BASE) & ~((0xFu << 4) | (0xFu << 8));
    GPIO_AFRH(GPIOA_BASE) = v | (7u << 4) | (7u << 8);
    RCC_APB2ENR |= (1u << 14);
    USART_CR1 &= ~1u;
    USART_BRR = 96000000u / 115200u;
    USART_CR1 |= (1u << 0) | (1u << 2) | (1u << 3);
}

/* hal/stm32u3.c clock_96mhz() */
static void clock_96mhz(void)
{
    uint32_t v;
    RCC_AHB1ENR2 |= (1u << 2);
    if ((PWR_SVMSR & (1u << 1)) == 0u) {
        PWR_CR3 |= (1u << 1);
        while ((PWR_SVMSR & (1u << 1)) == 0u) {
        }
    }
    RCC_CFGR4 = (RCC_CFGR4 & ~(0x3u | (0xFu << 12))) | 1u;
    if ((PWR_VOSR & (1u << 24)) == 0u) {
        PWR_VOSR |= (1u << 8);
        while ((PWR_VOSR & (1u << 24)) == 0u) {
        }
    }
    if ((PWR_VOSR & (1u << 16)) == 0u) {
        v = PWR_VOSR & ~3u;
        PWR_VOSR = v | 1u;
        while ((PWR_VOSR & (1u << 16)) == 0u) {
        }
    }
    FLASH_ACR = (FLASH_ACR & ~0xFu) | 2u;
    while ((FLASH_ACR & 0xFu) != 2u) {
    }
    FLASH_ACR |= (1u << 8);
    v = RCC_ICSCR1 & ~((1u << 31) | (3u << 29));
    RCC_ICSCR1 = v | (1u << 23);
    while ((RCC_CR & (1u << 2)) == 0u) {
    }
    RCC_CFGR2 = 0;
}

/* hal/stm32u3.c hal_cache_enable() / hal_cache_invalidate() */
static void icache_test(void)
{
    ICACHE_CR |= (1u << 2);
    ICACHE_CR |= 1u;
    check((ICACHE_CR & 1u) != 0u, "ICACHE enabled");
    if ((ICACHE_SR & 1u) == 0u) {
        ICACHE_CR |= (1u << 1);
    }
    while ((ICACHE_SR & (1u << 1)) == 0u) {
    }
    check((ICACHE_CR & (1u << 1)) == 0u, "CACHEINV self-clears");
    ICACHE_FCR = (1u << 1);
    check((ICACHE_SR & (1u << 1)) == 0u, "FCR clears BSYENDF");
}

static void flash_wait(void)
{
    while ((FLASH_NSSR & (SR_BSY | SR_WDW)) != 0u) {
    }
}

static void flash_unlock(void)
{
    if ((FLASH_NSCR & CR_LOCK) != 0u) {
        FLASH_NSKEYR = FLASH_KEY1;
        FLASH_NSKEYR = FLASH_KEY2;
    }
}

static void flash_erase_page(uint32_t bank2, uint32_t page)
{
    uint32_t v;
    flash_wait();
    FLASH_NSSR = SR_ERRORS;
    v = FLASH_NSCR & ~((0x7Fu << CR_PNB_SHIFT) | CR_BKER);
    v |= (page << CR_PNB_SHIFT) | CR_PER | (bank2 ? CR_BKER : 0u);
    FLASH_NSCR = v;
    FLASH_NSCR |= CR_STRT;
    flash_wait();
    FLASH_NSCR &= ~CR_PER;
}

/* hal/stm32u3.c hal_flash_write(): one double word */
static void flash_program(uint32_t addr, uint32_t lo, uint32_t hi)
{
    volatile uint32_t *dst = (volatile uint32_t *)addr;
    flash_wait();
    FLASH_NSCR |= CR_PG;
    dst[0] = lo;
    dst[1] = hi;
    flash_wait();
    FLASH_NSCR &= ~CR_PG;
}

static void flash_test(void)
{
    const volatile uint32_t *p = (const volatile uint32_t *)TEST_ADDR;

    check((FLASH_NSCR & CR_LOCK) != 0u, "flash locked after reset");
    flash_unlock();
    check((FLASH_NSCR & CR_LOCK) == 0u, "NSKEYR sequence unlocks NSCR");

    flash_erase_page(1u, TEST_PAGE);
    check((FLASH_NSSR & SR_EOP) != 0u, "bank 2 page erase sets EOP");
    FLASH_NSSR = SR_EOP;
    check(p[0] == 0xFFFFFFFFu && p[1] == 0xFFFFFFFFu, "erased page reads 0xFF");

    flash_program(TEST_ADDR, 0x11223344u, 0x55667788u);
    check((FLASH_NSSR & SR_ERRORS) == 0u, "program raises no error");
    check(p[0] == 0x11223344u && p[1] == 0x55667788u, "double word reads back");

    flash_program(TEST_ADDR, 0u, 0xFFFFFFFEu);
    check((FLASH_NSSR & SR_PROGERR) != 0u, "second program of a double word is PROGERR");
    check(p[0] == 0x11223344u, "refused program leaves the word alone");
    FLASH_NSSR = SR_ERRORS;
    check((FLASH_NSSR & SR_ERRORS) == 0u, "error flags clear on write 1");

    FLASH_NSCR |= CR_PG;
    *(volatile uint32_t *)(TEST_ADDR + 8u) = 0x12345678u;
    check((FLASH_NSSR & SR_WDW) != 0u, "half a double word sets WDW");
    *(volatile uint32_t *)(TEST_ADDR + 24u) = 0x12345678u;
    check((FLASH_NSSR & SR_PGSERR) != 0u, "store to another word with WDW is PGSERR");
    check(p[2] == 0xFFFFFFFFu && p[6] == 0xFFFFFFFFu, "both stores are lost");
    FLASH_NSSR = SR_ERRORS;
    FLASH_NSCR &= ~CR_PG;

    /* an erase of the neighbouring page must leave the test word alone */
    flash_erase_page(1u, TEST_PAGE - 1u);
    check(p[0] == 0x11223344u, "erasing the neighbouring page keeps the word");

    FLASH_NSCR |= CR_LOCK;
    check((FLASH_NSCR & CR_LOCK) != 0u, "LOCK set by software");
    FLASH_NSCR |= CR_PG;
    check((FLASH_NSCR & CR_PG) == 0u, "NSCR ignores writes while locked");
    *(volatile uint32_t *)(TEST_ADDR + 16u) = 0u;
    check(p[4] == 0xFFFFFFFFu, "store while locked is ignored");
    FLASH_NSSR = SR_ERRORS;
}

int main(void)
{
    clock_96mhz();
    console_init();
    printf("STM32U385 test start\n");
    check((RCC_CR & (1u << 2)) != 0u, "MSIS ready");
    check((PWR_VOSR & (1u << 16)) != 0u, "Range 1 ready");
    icache_test();
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
