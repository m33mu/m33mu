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

/* STM32C5A3 bring-up as wolfBoot's hal/stm32c5.c does it: HSE -> PSI ->
 * PSIS at 144 MHz, ICACHE, USART2 on PA2/PA3, then the flash controller:
 * bank 2 page erase, 128-bit programming through the write buffer, the
 * write-once rule, the lock, and the SWAP_BANK option change.  Run with
 * --dualbank. */

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

#define RCC_BASE        0x44020C00u
#define RCC_CR1         REG(RCC_BASE + 0x000u)
#define RCC_CR2         REG(RCC_BASE + 0x004u)
#define RCC_CFGR1       REG(RCC_BASE + 0x01Cu)
#define RCC_CFGR2       REG(RCC_BASE + 0x020u)
#define RCC_AHB2ENR     REG(RCC_BASE + 0x08Cu)
#define RCC_APB1LENR    REG(RCC_BASE + 0x09Cu)

#define ICACHE_BASE     0x40030400u
#define ICACHE_CR       REG(ICACHE_BASE + 0x000u)
#define ICACHE_SR       REG(ICACHE_BASE + 0x004u)
#define ICACHE_FCR      REG(ICACHE_BASE + 0x00Cu)

#define FLASH_BASE      0x40022000u
#define FLASH_ACR       REG(FLASH_BASE + 0x000u)
#define FLASH_KEYR      REG(FLASH_BASE + 0x004u)
#define FLASH_OPTKEYR   REG(FLASH_BASE + 0x00Cu)
#define FLASH_OPTCR     REG(FLASH_BASE + 0x01Cu)
#define FLASH_SR        REG(FLASH_BASE + 0x020u)
#define FLASH_CR        REG(FLASH_BASE + 0x028u)
#define FLASH_CCR       REG(FLASH_BASE + 0x030u)
#define FLASH_OPTSR_CUR REG(FLASH_BASE + 0x050u)
#define FLASH_OPTSR_PRG REG(FLASH_BASE + 0x054u)

#define FLASH_KEY1      0x45670123u
#define FLASH_KEY2      0xCDEF89ABu
#define FLASH_OPTKEY1   0x08192A3Bu
#define FLASH_OPTKEY2   0x4C5D6E7Fu
#define SR_BSY          (1u << 0)
#define SR_WBNE         (1u << 1)
#define SR_DBNE         (1u << 3)
#define SR_EOP          (1u << 16)
#define SR_PGSERR       (1u << 18)
#define SR_INCERR       (1u << 20)
#define SR_ERRORS       0x009E0000u
#define CR_LOCK         (1u << 0)
#define CR_PG           (1u << 1)
#define CR_PER          (1u << 2)
#define CR_FW           (1u << 4)
#define CR_STRT         (1u << 5)
#define CR_PNB_SHIFT    6
#define CR_BKSEL        (1u << 31)
#define OPTCR_OPTLOCK   (1u << 0)
#define OPTCR_OPTSTRT   (1u << 1)
#define SWAP_BANK       (1u << 31)

#define TEST_PAGE       63u
#define TEST_ADDR       (0x08080000u + TEST_PAGE * 0x2000u)

#define GPIOA_BASE      0x42020000u
#define GPIO_MODER(b)   REG((b) + 0x00u)
#define GPIO_AFRL(b)    REG((b) + 0x20u)

#define USART2_BASE     0x40004400u
#define USART_CR1       REG(USART2_BASE + 0x00u)
#define USART_BRR       REG(USART2_BASE + 0x0Cu)
#define USART_ISR       REG(USART2_BASE + 0x1Cu)
#define USART_TDR       REG(USART2_BASE + 0x28u)
#define ISR_TC          (1u << 6)
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
    RCC_AHB2ENR |= (1u << 0);
    v = GPIO_MODER(GPIOA_BASE) & ~((3u << 4) | (3u << 6));
    GPIO_MODER(GPIOA_BASE) = v | (2u << 4) | (2u << 6);
    v = GPIO_AFRL(GPIOA_BASE) & ~((0xFu << 8) | (0xFu << 12));
    GPIO_AFRL(GPIOA_BASE) = v | (7u << 8) | (7u << 12);
    RCC_APB1LENR |= (1u << 17);
    USART_CR1 &= ~1u;
    USART_BRR = 144000000u / 115200u;
    USART_CR1 |= (1u << 0) | (1u << 2) | (1u << 3);
}

/* hal/stm32c5.c clock_psi_on() */
static void clock_144mhz(void)
{
    uint32_t v;
    RCC_CR1 |= (1u << 16);
    while ((RCC_CR1 & (1u << 17)) == 0u) {
    }
    v = RCC_CR2 & ~((3u << 16) | (7u << 20) | (3u << 28));
    RCC_CR2 = v | (6u << 20) | (1u << 28);
    RCC_CR1 |= (1u << 8);
    while ((RCC_CR1 & (1u << 12)) == 0u) {
    }
    RCC_CFGR2 = 0;
    FLASH_ACR = (FLASH_ACR & ~0xFu) | 4u;
    while ((FLASH_ACR & 0xFu) != 4u) {
    }
    FLASH_ACR |= (1u << 8);
    RCC_CFGR1 = (RCC_CFGR1 & ~3u) | 3u;
    while (((RCC_CFGR1 >> 3) & 3u) != 3u) {
    }
    FLASH_ACR = (FLASH_ACR & ~(3u << 4)) | (2u << 4);
}

/* hal/stm32c5.c hal_cache_enable() / hal_cache_invalidate() */
static void icache_test(void)
{
    ICACHE_CR = (ICACHE_CR & ~(1u << 2)) | (1u << 2);
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
    while ((FLASH_SR & (SR_BSY | SR_WBNE | SR_DBNE)) != 0u) {
    }
}

static void flash_unlock(void)
{
    if ((FLASH_CR & CR_LOCK) != 0u) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
}

static void flash_erase_page(uint32_t bank2, uint32_t page)
{
    uint32_t v;
    flash_wait();
    FLASH_CCR = SR_ERRORS | SR_EOP;
    v = FLASH_CR & ~((0x3Fu << CR_PNB_SHIFT) | CR_BKSEL);
    v |= (page << CR_PNB_SHIFT) | CR_PER | (bank2 ? CR_BKSEL : 0u);
    FLASH_CR = v;
    FLASH_CR |= CR_STRT;
    flash_wait();
    FLASH_CR &= ~CR_PER;
}

/* hal/stm32c5.c hal_flash_write(): one 128-bit quad word */
static void flash_program(uint32_t addr, const uint32_t *w)
{
    volatile uint32_t *dst = (volatile uint32_t *)addr;
    flash_wait();
    FLASH_CR |= CR_PG;
    dst[0] = w[0];
    dst[1] = w[1];
    dst[2] = w[2];
    dst[3] = w[3];
    flash_wait();
    FLASH_CR &= ~CR_PG;
}

static void flash_test(void)
{
    const volatile uint32_t *p = (const volatile uint32_t *)TEST_ADDR;
    static const uint32_t data[4] = { 0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u };
    static const uint32_t again[4] = { 0u, 0u, 0u, 0xFFFFFFFEu };

    check((FLASH_CR & CR_LOCK) != 0u, "flash locked after reset");
    flash_unlock();
    check((FLASH_CR & CR_LOCK) == 0u, "KEYR sequence unlocks CR");

    flash_erase_page(1u, TEST_PAGE);
    check((FLASH_SR & SR_EOP) != 0u, "bank 2 page erase sets EOP");
    FLASH_CCR = SR_EOP;
    check((FLASH_SR & SR_EOP) == 0u, "CCR clears EOP");
    check(p[0] == 0xFFFFFFFFu && p[3] == 0xFFFFFFFFu, "erased page reads 0xFF");

    flash_program(TEST_ADDR, data);
    check((FLASH_SR & SR_WBNE) == 0u, "WBNE clear once the quad word is in");
    check((FLASH_SR & SR_ERRORS) == 0u, "program raises no error");
    check(p[0] == data[0] && p[1] == data[1] && p[2] == data[2] && p[3] == data[3],
          "quad word reads back");

    flash_program(TEST_ADDR, again);
    check((FLASH_SR & SR_PGSERR) != 0u, "second program of a quad word is PGSERR");
    check(p[0] == data[0] && p[3] == data[3], "refused program leaves the word alone");
    FLASH_CCR = SR_ERRORS;
    check((FLASH_SR & SR_ERRORS) == 0u, "CCR clears the error flags");

    FLASH_CR |= CR_PG;
    *(volatile uint32_t *)(TEST_ADDR + 16u) = 0x12345678u;
    check((FLASH_SR & SR_WBNE) != 0u, "partial quad word sets WBNE");
    *(volatile uint32_t *)(TEST_ADDR + 48u) = 0x12345678u;
    check((FLASH_SR & SR_INCERR) != 0u, "store to another word with WBNE is INCERR");
    check(p[4] == 0xFFFFFFFFu && p[12] == 0xFFFFFFFFu, "both stores are lost");
    FLASH_CCR = SR_ERRORS;
    *(volatile uint32_t *)(TEST_ADDR + 32u) = 0xCAFEF00Du;
    FLASH_CR |= CR_FW;
    flash_wait();
    check(p[8] == 0xCAFEF00Du && p[9] == 0xFFFFFFFFu, "FW programs a partial quad word");
    FLASH_CR &= ~CR_PG;

    /* an erase of the neighbouring page must leave the test word alone */
    flash_erase_page(1u, TEST_PAGE - 1u);
    check(p[0] == data[0], "erasing the neighbouring page keeps the word");

    FLASH_CR |= CR_LOCK;
    check((FLASH_CR & CR_LOCK) != 0u, "LOCK set by software");
    FLASH_CR |= CR_PG;
    check((FLASH_CR & CR_PG) == 0u, "CR ignores writes while locked");
    *(volatile uint32_t *)(TEST_ADDR + 64u) = 0u;
    check(p[16] == 0xFFFFFFFFu, "store while locked is ignored");
    FLASH_CCR = SR_ERRORS;
}

/* hal/stm32c5.c hal_flash_dualbank_swap(), short of the reboot */
static void option_test(void)
{
    check((FLASH_OPTCR & OPTCR_OPTLOCK) != 0u, "option bytes locked after reset");
    FLASH_OPTSR_PRG |= SWAP_BANK;
    check((FLASH_OPTSR_PRG & SWAP_BANK) == 0u, "OPTSR_PRG ignores writes while locked");
    FLASH_OPTKEYR = FLASH_OPTKEY1;
    FLASH_OPTKEYR = FLASH_OPTKEY2;
    check((FLASH_OPTCR & OPTCR_OPTLOCK) == 0u, "OPTKEYR sequence unlocks OPTCR");
    FLASH_OPTSR_PRG |= SWAP_BANK;
    FLASH_OPTCR |= OPTCR_OPTSTRT;
    flash_wait();
    check((FLASH_OPTCR & OPTCR_OPTSTRT) == 0u, "OPTSTRT self-clears");
    check((FLASH_OPTSR_CUR & SWAP_BANK) != 0u, "OPTSTRT makes SWAP_BANK the option in force");
    check((FLASH_OPTCR & SWAP_BANK) == 0u, "the banks swap only at the next reset");
    FLASH_OPTSR_PRG &= ~SWAP_BANK;
    FLASH_OPTCR |= OPTCR_OPTSTRT;
    flash_wait();
    check((FLASH_OPTSR_CUR & SWAP_BANK) == 0u, "SWAP_BANK option cleared again");
    FLASH_OPTCR |= OPTCR_OPTLOCK;
    check((FLASH_OPTCR & OPTCR_OPTLOCK) != 0u, "OPTLOCK set by software");
}

int main(void)
{
    clock_144mhz();
    console_init();
    printf("STM32C5A3 test start\n");
    check(((RCC_CFGR1 >> 3) & 3u) == 3u, "SYSCLK from PSIS");
    icache_test();
    flash_test();
    option_test();
    while ((USART_ISR & ISR_TC) == 0u) {
    }
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
