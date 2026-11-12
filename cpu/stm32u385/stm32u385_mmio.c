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

/* STM32U385 system peripherals (RM0487): RCC, PWR, the embedded flash
 * controller, ICACHE and GPIO.  The flash is two 512 KB banks of 4 KB
 * pages programmed by 64-bit double words; the system peripherals sit on
 * AHB1 at 0x40030000, not at the STM32U5 addresses. */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "stm32u385/stm32u385_mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/gpio.h"
#include "m33mu/nvic.h"
#include "stm32_gpio.h"

extern void mm_system_request_reset(void);

#define RCC_BASE       0x40030C00u
#define RCC_SEC_BASE   0x50030C00u
#define RCC_SIZE       0x400u
#define RCC_CR         0x000u
#define RCC_ICSCR1     0x008u
#define RCC_CFGR1      0x01Cu
#define RCC_CFGR2      0x020u
#define RCC_AHB2ENR1   0x08Cu
#define RCC_BDCR       0x110u
#define RCC_CSR        0x114u

#define RCC_CR_MSISON     (1u << 0)
#define RCC_CR_MSISRDY    (1u << 2)
#define RCC_CR_MSIKON     (1u << 3)
#define RCC_CR_MSIKRDY    (1u << 4)
#define RCC_CR_MSIPLL1EN  (1u << 5)
#define RCC_CR_MSIPLL0EN  (1u << 6)
#define RCC_CR_MSIPLL1RDY (1u << 9)
#define RCC_CR_MSIPLL0RDY (1u << 10)
#define RCC_CR_HSION      (1u << 11)
#define RCC_CR_HSIRDY     (1u << 13)
#define RCC_CR_HSI48ON    (1u << 14)
#define RCC_CR_HSI48RDY   (1u << 15)
#define RCC_CR_HSEON      (1u << 16)
#define RCC_CR_HSERDY     (1u << 17)
#define RCC_ICSCR1_MSIRGSEL (1u << 23)
#define RCC_ICSCR1_MSISSEL  (1u << 31)
#define RCC_BDCR_LSEON    (1u << 0)
#define RCC_BDCR_LSERDY   (1u << 1)
#define RCC_BDCR_LSION    (1u << 26)
#define RCC_BDCR_LSIRDY   (1u << 27)
/* MSIS out of reset: MSIRC1 (24 MHz) divided by 2 (RCC_CSR.MSISDIVS) */
#define RCC_CSR_RESET     0x00001000u

#define PWR_BASE       0x40030800u
#define PWR_SEC_BASE   0x50030800u
#define PWR_SIZE       0x400u
#define PWR_CR3        0x008u
#define PWR_VOSR       0x00Cu
#define PWR_SVMSR      0x03Cu
#define PWR_CR3_REGSEL    (1u << 1)
#define PWR_VOSR_R1EN     (1u << 0)
#define PWR_VOSR_R2EN     (1u << 1)
#define PWR_VOSR_BOOSTEN  (1u << 8)
#define PWR_VOSR_R1RDY    (1u << 16)
#define PWR_VOSR_R2RDY    (1u << 17)
#define PWR_VOSR_BOOSTRDY (1u << 24)
#define PWR_SVMSR_REGS    (1u << 1)

#define FLASH_BASE     0x40022000u
#define FLASH_SEC_BASE 0x50022000u
#define FLASH_SIZE     0x400u
#define FLASH_ACR      0x000u
#define FLASH_NSKEYR   0x008u
#define FLASH_SECKEYR  0x00Cu
#define FLASH_OPTKEYR  0x010u
#define FLASH_NSSR     0x020u
#define FLASH_SECSR    0x024u
#define FLASH_NSCR     0x028u
#define FLASH_SECCR    0x02Cu
#define FLASH_OPTR     0x040u

#define FLASH_KEY1     0x45670123u
#define FLASH_KEY2     0xCDEF89ABu
#define FLASH_OPTKEY1  0x08192A3Bu
#define FLASH_OPTKEY2  0x4C5D6E7Fu

#define FLASH_SR_EOP     (1u << 0)
#define FLASH_SR_OPERR   (1u << 1)
#define FLASH_SR_PROGERR (1u << 3)
#define FLASH_SR_WRPERR  (1u << 4)
#define FLASH_SR_PGAERR  (1u << 5)
#define FLASH_SR_SIZERR  (1u << 6)
#define FLASH_SR_PGSERR  (1u << 7)
#define FLASH_SR_OPTWERR (1u << 13)
#define FLASH_SR_BSY     (1u << 16)
#define FLASH_SR_WDW     (1u << 17)
#define FLASH_SR_W1C     (FLASH_SR_EOP | FLASH_SR_OPERR | FLASH_SR_PROGERR | \
                          FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_SIZERR | \
                          FLASH_SR_PGSERR | FLASH_SR_OPTWERR)

#define FLASH_CR_PG      (1u << 0)
#define FLASH_CR_PER     (1u << 1)
#define FLASH_CR_MER1    (1u << 2)
#define FLASH_CR_PNB_SHIFT 3
#define FLASH_CR_PNB_MASK (0x7Fu << FLASH_CR_PNB_SHIFT)
#define FLASH_CR_BKER    (1u << 11)
#define FLASH_CR_MER2    (1u << 15)
#define FLASH_CR_STRT    (1u << 16)
#define FLASH_CR_OPTSTRT (1u << 17)
#define FLASH_CR_OBL_LAUNCH (1u << 27)
#define FLASH_CR_OPTLOCK (1u << 30)
#define FLASH_CR_LOCK    (1u << 31)

#define FLASH_PAGE_SIZE  0x1000u
#define FLASH_BANK_COUNT 2u
#define FLASH_WORD_SIZE  8u
#define FLASH_OPTR_DUALBANK (1u << 21)
#define FLASH_OPTR_RESET (FLASH_OPTR_DUALBANK | 0xAAu) /* RDP level 0, TZEN=0 */

#define ICACHE_BASE     0x40030400u
#define ICACHE_SEC_BASE 0x50030400u
#define ICACHE_SIZE     0x400u
#define ICACHE_CR       0x000u
#define ICACHE_SR       0x004u
#define ICACHE_FCR      0x00Cu
#define ICACHE_CR_CACHEINV (1u << 1)
#define ICACHE_SR_BSYENDF  (1u << 1)
#define ICACHE_SR_ERRF     (1u << 2)
#define ICACHE_CR_RESET    0x00000004u

#define GPIO_BASE_NS    0x42020000u
#define GPIO_BASE_S     0x52020000u
#define GPIO_BANK_COUNT 8  /* A..E, G and H are present */

struct rcc_state {
    mm_u32 regs[RCC_SIZE / 4];
    mm_u64 cpu_hz;
};

struct pwr_state {
    mm_u32 regs[PWR_SIZE / 4];
};

struct icache_state {
    mm_u32 regs[ICACHE_SIZE / 4];
};

struct flash_state {
    mm_u32 regs[FLASH_SIZE / 4];
    mm_u8 *flash;
    mm_u32 flash_size;
    mm_u32 base_s;
    mm_u32 base_ns;
    const struct mm_flash_persist *persist;
    mm_u32 flags;
    mm_u8 ns_key_stage;
    mm_u8 sec_key_stage;
    mm_u8 opt_key_stage;
    /* the double word being assembled from consecutive stores (NSSR.WDW) */
    mm_bool wb_valid;
    mm_u32 wb_off;
    mm_u32 wb_mask;
    mm_u32 wb_sr_off;
    mm_u8 wb_data[FLASH_WORD_SIZE];
};

static struct rcc_state rcc;
static struct pwr_state pwr;
static struct icache_state icache;
static struct flash_state flash_ctl;
static struct stm32_gpio_state gpio[GPIO_BANK_COUNT];
static struct stm32_gpio_ctx gpio_ctx_data[GPIO_BANK_COUNT * 2];
static const int gpio_present[] = { 0, 1, 2, 3, 4, 6, 7 };

static mm_bool flash_trace_enabled(void)
{
    static mm_bool init = MM_FALSE;
    static mm_bool enabled = MM_FALSE;
    if (!init) {
        const char *env = getenv("M33MU_FLASH_TRACE");
        enabled = (env != 0 && env[0] != '\0') ? MM_TRUE : MM_FALSE;
        init = MM_TRUE;
    }
    return enabled;
}

/* ---- RCC ---- */

static void rcc_update_ready(struct rcc_state *r)
{
    mm_u32 cr = r->regs[RCC_CR / 4];
    mm_u32 bdcr = r->regs[RCC_BDCR / 4];
    /* oscillators and the MSI PLLs are ready as soon as they are on */
    if ((cr & RCC_CR_MSISON) != 0u) cr |= RCC_CR_MSISRDY; else cr &= ~RCC_CR_MSISRDY;
    if ((cr & RCC_CR_MSIKON) != 0u) cr |= RCC_CR_MSIKRDY; else cr &= ~RCC_CR_MSIKRDY;
    if ((cr & RCC_CR_MSIPLL1EN) != 0u) cr |= RCC_CR_MSIPLL1RDY; else cr &= ~RCC_CR_MSIPLL1RDY;
    if ((cr & RCC_CR_MSIPLL0EN) != 0u) cr |= RCC_CR_MSIPLL0RDY; else cr &= ~RCC_CR_MSIPLL0RDY;
    if ((cr & RCC_CR_HSION) != 0u) cr |= RCC_CR_HSIRDY; else cr &= ~RCC_CR_HSIRDY;
    if ((cr & RCC_CR_HSI48ON) != 0u) cr |= RCC_CR_HSI48RDY; else cr &= ~RCC_CR_HSI48RDY;
    if ((cr & RCC_CR_HSEON) != 0u) cr |= RCC_CR_HSERDY; else cr &= ~RCC_CR_HSERDY;
    r->regs[RCC_CR / 4] = cr;
    if ((bdcr & RCC_BDCR_LSEON) != 0u) bdcr |= RCC_BDCR_LSERDY; else bdcr &= ~RCC_BDCR_LSERDY;
    if ((bdcr & RCC_BDCR_LSION) != 0u) bdcr |= RCC_BDCR_LSIRDY; else bdcr &= ~RCC_BDCR_LSIRDY;
    r->regs[RCC_BDCR / 4] = bdcr;
}

/* MSIS: MSIRC0 (96 MHz) or MSIRC1 (24 MHz) through MSISDIV, taken from
 * ICSCR1 once MSIRGSEL is set and from the CSR reset copy before. */
static mm_u64 rcc_msis_hz(const struct rcc_state *r)
{
    mm_u32 icscr1 = r->regs[RCC_ICSCR1 / 4];
    mm_u32 sel;
    mm_u32 div;
    if ((icscr1 & RCC_ICSCR1_MSIRGSEL) != 0u) {
        sel = (icscr1 & RCC_ICSCR1_MSISSEL) != 0u ? 1u : 0u;
        div = (icscr1 >> 29) & 0x3u;
    } else {
        mm_u32 csr = r->regs[RCC_CSR / 4];
        sel = (csr >> 15) & 0x1u;
        div = (csr >> 12) & 0x3u;
    }
    return ((sel != 0u) ? 24000000ull : 96000000ull) >> div;
}

static void rcc_update_sysclk(struct rcc_state *r)
{
    mm_u32 cfgr1 = r->regs[RCC_CFGR1 / 4];
    mm_u32 hpre = r->regs[RCC_CFGR2 / 4] & 0xFu;
    mm_u32 sw = cfgr1 & 0x3u;
    mm_u64 sys = 0;
    mm_u32 div = 1u;

    if (sw == 0u) sys = rcc_msis_hz(r);
    else if (sw == 1u) sys = 16000000ull;
    else if (sw == 2u) sys = 16000000ull;        /* HSE: NUCLEO-U385RG-Q crystal */
    if (hpre >= 8u) {
        div = (hpre <= 11u) ? (2u << (hpre - 8u)) : (64u << (hpre - 12u));
    }
    r->cpu_hz = (sys == 0) ? 0 : (sys / (mm_u64)div);
    if (sys != 0 && r->cpu_hz == 0) r->cpu_hz = 1;
    r->regs[RCC_CFGR1 / 4] = (cfgr1 & ~(0x3u << 2)) | (sw << 2);
}

static mm_bool rcc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rcc_state *r = (struct rcc_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RCC_SIZE) return MM_FALSE;
    if (offset <= RCC_CR && (offset + size_bytes) > RCC_CR) {
        rcc_update_ready(r);
    }
    if (offset <= RCC_CFGR1 && (offset + size_bytes) > RCC_CFGR1) {
        rcc_update_sysclk(r);
    }
    memcpy(value_out, (mm_u8 *)r->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool rcc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rcc_state *r = (struct rcc_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RCC_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)r->regs + offset, &value, size_bytes);
    if (offset == RCC_CR || offset == RCC_BDCR) {
        rcc_update_ready(r);
    }
    if (offset == RCC_CR || offset == RCC_ICSCR1 || offset == RCC_CFGR1 ||
        offset == RCC_CFGR2) {
        rcc_update_sysclk(r);
    }
    return MM_TRUE;
}

/* ---- PWR ---- */

/* The regulator and range switches complete at once: the ready bits follow
 * the enable bits, SVMSR.REGS follows CR3.REGSEL. */
static void pwr_update(struct pwr_state *p)
{
    mm_u32 vosr = p->regs[PWR_VOSR / 4];
    mm_u32 svmsr = p->regs[PWR_SVMSR / 4];
    vosr &= ~(PWR_VOSR_R1RDY | PWR_VOSR_R2RDY | PWR_VOSR_BOOSTRDY);
    if ((vosr & PWR_VOSR_R1EN) != 0u) vosr |= PWR_VOSR_R1RDY;
    if ((vosr & PWR_VOSR_R2EN) != 0u) vosr |= PWR_VOSR_R2RDY;
    if ((vosr & PWR_VOSR_BOOSTEN) != 0u) vosr |= PWR_VOSR_BOOSTRDY;
    svmsr &= ~PWR_SVMSR_REGS;
    if ((p->regs[PWR_CR3 / 4] & PWR_CR3_REGSEL) != 0u) svmsr |= PWR_SVMSR_REGS;
    p->regs[PWR_VOSR / 4] = vosr;
    p->regs[PWR_SVMSR / 4] = svmsr;
}

static mm_bool pwr_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct pwr_state *p = (struct pwr_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > PWR_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)p->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool pwr_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct pwr_state *p = (struct pwr_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > PWR_SIZE) return MM_FALSE;
    if (offset == PWR_SVMSR) {
        return MM_TRUE;                             /* status: read-only */
    }
    memcpy((mm_u8 *)p->regs + offset, &value, size_bytes);
    if (offset == PWR_VOSR || offset == PWR_CR3) {
        pwr_update(p);
    }
    return MM_TRUE;
}

/* ---- ICACHE ---- */

static mm_bool icache_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct icache_state *c = (struct icache_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > ICACHE_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)c->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool icache_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct icache_state *c = (struct icache_state *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > ICACHE_SIZE) return MM_FALSE;
    if (offset == ICACHE_SR) {
        return MM_TRUE;                             /* read-only, cleared via FCR */
    }
    if (offset == ICACHE_FCR) {
        c->regs[ICACHE_SR / 4] &= ~(value & (ICACHE_SR_BSYENDF | ICACHE_SR_ERRF));
        return MM_TRUE;
    }
    memcpy((mm_u8 *)c->regs + offset, &value, size_bytes);
    if (offset == ICACHE_CR && (value & ICACHE_CR_CACHEINV) != 0u) {
        /* the invalidation completes at once: BUSYF is never seen set */
        c->regs[ICACHE_CR / 4] &= ~ICACHE_CR_CACHEINV;
        c->regs[ICACHE_SR / 4] |= ICACHE_SR_BSYENDF;
    }
    return MM_TRUE;
}

/* ---- FLASH ---- */

static void flash_set_eop(mm_u32 sr_off)
{
    flash_ctl.regs[sr_off / 4u] |= FLASH_SR_EOP;
}

static mm_bool flash_is_unlocked(mm_u32 cr_off)
{
    return (flash_ctl.regs[cr_off / 4u] & FLASH_CR_LOCK) == 0u;
}

static void flash_handle_key(mm_u32 offset, mm_u32 value)
{
    mm_u8 *stage = (offset == FLASH_NSKEYR) ? &flash_ctl.ns_key_stage : &flash_ctl.sec_key_stage;
    mm_u32 cr_off = (offset == FLASH_NSKEYR) ? FLASH_NSCR : FLASH_SECCR;
    if (*stage == 0u) {
        *stage = (value == FLASH_KEY1) ? 1u : 0u;
        return;
    }
    if (value == FLASH_KEY2) {
        flash_ctl.regs[cr_off / 4u] &= ~FLASH_CR_LOCK;
    }
    *stage = 0u;
}

/* OPTLOCK lives in both control registers; a wrong key keeps it set until reset */
static void flash_handle_opt_key(mm_u32 value)
{
    if (flash_ctl.opt_key_stage == 0u && value == FLASH_OPTKEY1) {
        flash_ctl.opt_key_stage = 1u;
    } else if (flash_ctl.opt_key_stage == 1u && value == FLASH_OPTKEY2) {
        flash_ctl.regs[FLASH_NSCR / 4u] &= ~FLASH_CR_OPTLOCK;
        flash_ctl.regs[FLASH_SECCR / 4u] &= ~FLASH_CR_OPTLOCK;
        flash_ctl.opt_key_stage = 0u;
    } else {
        flash_ctl.opt_key_stage = 2u;
    }
}

static void flash_wb_drop(void)
{
    flash_ctl.wb_valid = MM_FALSE;
    flash_ctl.wb_mask = 0u;
    flash_ctl.regs[FLASH_NSSR / 4u] &= ~FLASH_SR_WDW;
    flash_ctl.regs[FLASH_SECSR / 4u] &= ~FLASH_SR_WDW;
}

/* Program the assembled double word.  One that is not erased takes only
 * all-zero data (RM0487 7.3.7); anything else is PROGERR with nothing
 * written. */
static void flash_wb_commit(void)
{
    mm_u8 data[FLASH_WORD_SIZE];
    mm_u32 off = flash_ctl.wb_off;
    mm_u32 sr_off = flash_ctl.wb_sr_off;
    mm_bool blank = MM_TRUE;
    mm_bool zero = MM_TRUE;
    mm_u32 i;

    memcpy(data, flash_ctl.wb_data, sizeof(data));
    flash_wb_drop();
    for (i = 0; i < FLASH_WORD_SIZE; ++i) {
        if (flash_ctl.flash[off + i] != 0xFFu) blank = MM_FALSE;
        if (data[i] != 0x00u) zero = MM_FALSE;
    }
    if ((flash_ctl.flags & MM_TARGET_FLAG_NVM_WRITEONCE) != 0u && !blank && !zero) {
        flash_ctl.regs[sr_off / 4u] |= FLASH_SR_PROGERR;
        return;
    }
    for (i = 0; i < FLASH_WORD_SIZE; ++i) {
        flash_ctl.flash[off + i] &= data[i];
    }
    flash_set_eop(sr_off);
    if (flash_ctl.persist != 0 && flash_ctl.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)flash_ctl.persist, off, FLASH_WORD_SIZE);
    }
}

/* Stores with PG set fill one double word; WDW is up until the last byte
 * is in.  A store to another word first is a sequence error and loses both. */
static mm_bool flash_wb_store(mm_u32 offset, mm_u32 size_bytes, mm_u32 value, mm_u32 sr_off)
{
    mm_u32 word = offset & ~(FLASH_WORD_SIZE - 1u);
    mm_u32 at = offset - word;
    mm_u32 i;

    if ((size_bytes != 1u && size_bytes != 2u && size_bytes != 4u) || at + size_bytes > FLASH_WORD_SIZE) {
        return MM_FALSE;
    }
    if (flash_ctl.wb_valid && flash_ctl.wb_off != word) {
        flash_ctl.regs[sr_off / 4u] |= FLASH_SR_PGSERR;
        flash_wb_drop();
        return MM_TRUE;
    }
    if (!flash_ctl.wb_valid) {
        memset(flash_ctl.wb_data, 0xFF, sizeof(flash_ctl.wb_data));
        flash_ctl.wb_valid = MM_TRUE;
        flash_ctl.wb_off = word;
        flash_ctl.wb_mask = 0u;
        flash_ctl.wb_sr_off = sr_off;
    }
    for (i = 0; i < size_bytes; ++i) {
        flash_ctl.wb_data[at + i] = (mm_u8)((value >> (8u * i)) & 0xFFu);
        flash_ctl.wb_mask |= 1u << (at + i);
    }
    flash_ctl.regs[sr_off / 4u] |= FLASH_SR_WDW;
    if (flash_ctl.wb_mask == ((1u << FLASH_WORD_SIZE) - 1u)) {
        flash_wb_commit();
    }
    return MM_TRUE;
}

/* PNB counts pages within the bank BKER selects; MER1/MER2 erase a bank. */
static void flash_apply_erase(mm_u32 cr_off, mm_u32 sr_off)
{
    mm_u32 cr = flash_ctl.regs[cr_off / 4u];
    mm_u32 bank_size;
    mm_u32 start;
    mm_u32 length;
    if (flash_ctl.flash == 0 || flash_ctl.flash_size == 0) {
        return;
    }
    bank_size = flash_ctl.flash_size / FLASH_BANK_COUNT;
    if ((cr & (FLASH_CR_MER1 | FLASH_CR_MER2)) != 0u) {
        start = ((cr & FLASH_CR_MER1) != 0u) ? 0u : bank_size;
        length = ((cr & FLASH_CR_MER1) != 0u && (cr & FLASH_CR_MER2) != 0u) ?
            flash_ctl.flash_size : bank_size;
    } else if ((cr & FLASH_CR_PER) != 0u) {
        start = ((cr & FLASH_CR_PNB_MASK) >> FLASH_CR_PNB_SHIFT) * FLASH_PAGE_SIZE;
        if ((cr & FLASH_CR_BKER) != 0u) {
            start += bank_size;
        }
        length = FLASH_PAGE_SIZE;
        if (start >= flash_ctl.flash_size) {
            return;
        }
        if (start + length > flash_ctl.flash_size) {
            length = flash_ctl.flash_size - start;
        }
    } else {
        /* STRT with no erase mode selected */
        flash_ctl.regs[sr_off / 4u] |= FLASH_SR_PGSERR;
        return;
    }
    if (flash_trace_enabled()) {
        printf("[FLASH_ERASE] %s mode=%s pnb=%lu start=0x%08lx len=0x%08lx\n",
               (cr_off == FLASH_SECCR) ? "S" : "NS",
               (cr & (FLASH_CR_MER1 | FLASH_CR_MER2)) ? "MER" : "PER",
               (unsigned long)((cr & FLASH_CR_PNB_MASK) >> FLASH_CR_PNB_SHIFT),
               (unsigned long)start,
               (unsigned long)length);
    }
    memset(flash_ctl.flash + start, 0xFF, length);
    flash_set_eop(sr_off);
    if (flash_ctl.persist != 0 && flash_ctl.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)flash_ctl.persist, start, length);
    }
}

static mm_bool flash_write_cb(void *opaque, enum mm_sec_state sec, mm_u32 addr, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 base;
    mm_u32 offset;
    mm_u32 cr_off;
    mm_u32 sr_off;
    (void)opaque;

    if (flash_ctl.flash == 0 || flash_ctl.flash_size == 0) {
        return MM_FALSE;
    }
    if (addr >= flash_ctl.base_s && addr < flash_ctl.base_s + flash_ctl.flash_size) {
        base = flash_ctl.base_s;
    } else if (addr >= flash_ctl.base_ns && addr < flash_ctl.base_ns + flash_ctl.flash_size) {
        base = flash_ctl.base_ns;
    } else {
        return MM_FALSE;
    }
    offset = addr - base;
    cr_off = (sec == MM_SECURE) ? FLASH_SECCR : FLASH_NSCR;
    sr_off = (sec == MM_SECURE) ? FLASH_SECSR : FLASH_NSSR;
    if (offset + size_bytes > flash_ctl.flash_size) {
        return MM_FALSE;
    }
    if (flash_trace_enabled()) {
        printf("[FLASH_WRITE] %s addr=0x%08lx size=%lu value=0x%08lx\n",
               (sec == MM_SECURE) ? "S" : "NS",
               (unsigned long)addr,
               (unsigned long)size_bytes,
               (unsigned long)value);
    }
    if (!flash_is_unlocked(cr_off) || (flash_ctl.regs[cr_off / 4u] & FLASH_CR_PG) == 0u) {
        /* a store with PG clear is a programming sequence error */
        flash_ctl.regs[sr_off / 4u] |= FLASH_SR_PGSERR;
        return MM_TRUE;
    }
    return flash_wb_store(offset, size_bytes, value, sr_off);
}

static mm_bool flash_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct flash_state *f = (struct flash_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FLASH_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)f->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool flash_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct flash_state *f = (struct flash_state *)opaque;
    mm_u32 sr_off;
    mm_u32 old;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FLASH_SIZE) return MM_FALSE;

    if (offset == FLASH_NSKEYR || offset == FLASH_SECKEYR) {
        flash_handle_key(offset, value);
        return MM_TRUE;
    }
    if (offset == FLASH_OPTKEYR) {
        flash_handle_opt_key(value);
        return MM_TRUE;
    }
    if (offset == FLASH_NSSR || offset == FLASH_SECSR) {
        f->regs[offset / 4u] &= ~(value & FLASH_SR_W1C);
        return MM_TRUE;
    }
    if (offset == FLASH_OPTR) {
        return MM_TRUE;                             /* option bytes: loaded at reset */
    }
    if (offset == FLASH_NSCR || offset == FLASH_SECCR) {
        sr_off = (offset == FLASH_NSCR) ? FLASH_NSSR : FLASH_SECSR;
        old = f->regs[offset / 4u];
        if (!flash_is_unlocked(offset)) {
            return MM_TRUE;                         /* locked: every bit is read-only */
        }
        memcpy((mm_u8 *)f->regs + offset, &value, size_bytes);
        f->regs[offset / 4u] &= ~(FLASH_CR_STRT | FLASH_CR_OPTSTRT | FLASH_CR_OBL_LAUNCH);
        /* LOCK and OPTLOCK are set by software and cleared only by the keys */
        f->regs[offset / 4u] |= (old & FLASH_CR_OPTLOCK) | (value & FLASH_CR_LOCK);
        if ((old & FLASH_CR_PG) != 0u && (f->regs[offset / 4u] & FLASH_CR_PG) == 0u &&
            f->wb_valid) {
            f->regs[sr_off / 4u] |= FLASH_SR_PGSERR;    /* double word left incomplete */
            flash_wb_drop();
        }
        if ((value & FLASH_CR_STRT) != 0u) {
            if (f->wb_valid) {
                f->regs[sr_off / 4u] |= FLASH_SR_PGSERR;
            } else {
                flash_apply_erase(offset, sr_off);
            }
        }
        if ((value & FLASH_CR_OPTSTRT) != 0u && (old & FLASH_CR_OPTLOCK) != 0u) {
            f->regs[sr_off / 4u] |= FLASH_SR_OPTWERR;
        }
        if ((value & FLASH_CR_OBL_LAUNCH) != 0u && (old & FLASH_CR_OPTLOCK) == 0u) {
            mm_system_request_reset();              /* option byte loading resets the device */
        }
        return MM_TRUE;
    }
    memcpy((mm_u8 *)f->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

/* ---- GPIO ---- */

static mm_bool gpio_clock_enabled(int bank)
{
    if (bank < 0 || bank >= GPIO_BANK_COUNT) {
        return MM_FALSE;
    }
    return ((rcc.regs[RCC_AHB2ENR1 / 4] >> bank) & 1u) != 0u;
}

static mm_u32 gpio_bank_read(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= GPIO_BANK_COUNT) {
        return 0u;
    }
    return gpio[bank].regs[STM32_GPIO_ODR_OFFSET / 4];
}

static mm_u32 gpio_bank_read_moder(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= GPIO_BANK_COUNT) {
        return 0u;
    }
    return gpio[bank].regs[STM32_GPIO_MODER_OFFSET / 4];
}

static mm_bool gpio_bank_clock(void *opaque, int bank)
{
    (void)opaque;
    return gpio_clock_enabled(bank);
}

static mm_u32 gpio_bank_read_seccfgr(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= GPIO_BANK_COUNT) {
        return 0u;
    }
    return gpio[bank].regs[STM32_GPIO_SECCFGR_OFFSET / 4];
}

static mm_bool gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    size_t i;
    (void)opaque;
    for (i = 0; i < sizeof(gpio_present) / sizeof(gpio_present[0]); ++i) {
        if (gpio_present[i] == bank) {
            if (name_out != 0 && name_len > 0) {
                snprintf(name_out, name_len, "GPIO%c", 'A' + bank);
            }
            if (pins_out != 0) {
                *pins_out = 16;
            }
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static void gpio_reset_all(void)
{
    size_t i;
    for (i = 0; i < GPIO_BANK_COUNT; ++i) {
        stm32_gpio_reset(&gpio[i], (int)i);
        gpio_ctx_data[i * 2].gpio = &gpio[i];
        gpio_ctx_data[i * 2].is_secure_alias = MM_FALSE;
        gpio_ctx_data[i * 2].bank_index = (int)i;
        gpio_ctx_data[i * 2].clock_enabled = gpio_clock_enabled;
        gpio_ctx_data[i * 2].exti_update = 0;
        gpio_ctx_data[i * 2 + 1].gpio = &gpio[i];
        gpio_ctx_data[i * 2 + 1].is_secure_alias = MM_TRUE;
        gpio_ctx_data[i * 2 + 1].bank_index = (int)i;
        gpio_ctx_data[i * 2 + 1].clock_enabled = gpio_clock_enabled;
        gpio_ctx_data[i * 2 + 1].exti_update = 0;
    }
    mm_gpio_bank_set_reader(gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(gpio_bank_clock, 0);
    mm_gpio_bank_set_seccfgr_reader(gpio_bank_read_seccfgr, 0);
    mm_gpio_set_bank_info_reader(gpio_bank_info, 0);
}

/* ---- target hooks ---- */

static void soc_reset_regs(void)
{
    memset(&rcc, 0, sizeof(rcc));
    memset(&pwr, 0, sizeof(pwr));
    memset(&icache, 0, sizeof(icache));
    memset(&flash_ctl, 0, sizeof(flash_ctl));
    gpio_reset_all();
    /* MSIS at 12 MHz drives SYSCLK out of reset (RM0487 RCC_CR, RCC_CSR) */
    rcc.regs[RCC_CR / 4u] = RCC_CR_MSISON;
    rcc.regs[RCC_CSR / 4u] = RCC_CSR_RESET;
    rcc_update_ready(&rcc);
    rcc_update_sysclk(&rcc);
    pwr.regs[PWR_VOSR / 4u] = PWR_VOSR_R2EN;        /* Range 2, LDO */
    pwr_update(&pwr);
    icache.regs[ICACHE_CR / 4u] = ICACHE_CR_RESET;
    flash_ctl.regs[FLASH_NSCR / 4u] = FLASH_CR_LOCK | FLASH_CR_OPTLOCK;
    flash_ctl.regs[FLASH_SECCR / 4u] = FLASH_CR_LOCK | FLASH_CR_OPTLOCK;
    flash_ctl.regs[FLASH_OPTR / 4u] = FLASH_OPTR_RESET;
}

void mm_stm32u385_mmio_reset(void)
{
    soc_reset_regs();
}

mm_u32 *mm_stm32u385_rcc_regs(void)
{
    return rcc.regs;
}

mm_u64 mm_stm32u385_cpu_hz(void)
{
    return rcc.cpu_hz;
}

static mm_bool register_pair(struct mmio_bus *bus, struct mmio_region *reg, mm_u32 base_ns, mm_u32 base_s)
{
    reg->base = base_ns;
    if (!mmio_bus_register_region(bus, reg)) return MM_FALSE;
    reg->base = base_s;
    return mmio_bus_register_region(bus, reg);
}

mm_bool mm_stm32u385_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    size_t i;

    soc_reset_regs();
    memset(&reg, 0, sizeof(reg));

    reg.size = RCC_SIZE;
    reg.opaque = &rcc;
    reg.read = rcc_read;
    reg.write = rcc_write;
    if (!register_pair(bus, &reg, RCC_BASE, RCC_SEC_BASE)) return MM_FALSE;

    reg.size = PWR_SIZE;
    reg.opaque = &pwr;
    reg.read = pwr_read;
    reg.write = pwr_write;
    if (!register_pair(bus, &reg, PWR_BASE, PWR_SEC_BASE)) return MM_FALSE;

    reg.size = FLASH_SIZE;
    reg.opaque = &flash_ctl;
    reg.read = flash_read;
    reg.write = flash_write;
    if (!register_pair(bus, &reg, FLASH_BASE, FLASH_SEC_BASE)) return MM_FALSE;

    reg.size = ICACHE_SIZE;
    reg.opaque = &icache;
    reg.read = icache_read;
    reg.write = icache_write;
    if (!register_pair(bus, &reg, ICACHE_BASE, ICACHE_SEC_BASE)) return MM_FALSE;

    reg.size = STM32_GPIO_REG_SIZE;
    reg.read = stm32_gpio_read;
    reg.write = stm32_gpio_write;
    for (i = 0; i < sizeof(gpio_present) / sizeof(gpio_present[0]); ++i) {
        int bank = gpio_present[i];
        reg.base = GPIO_BASE_NS + (mm_u32)bank * 0x400u;
        reg.opaque = &gpio_ctx_data[bank * 2];
        if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
        reg.base = GPIO_BASE_S + (mm_u32)bank * 0x400u;
        reg.opaque = &gpio_ctx_data[bank * 2 + 1];
        if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    }
    return MM_TRUE;
}

void mm_stm32u385_flash_bind(struct mm_memmap *map,
                             mm_u8 *flash,
                             mm_u32 flash_size,
                             const struct mm_flash_persist *persist,
                             mm_u32 flags)
{
    if (map == 0) {
        return;
    }
    flash_ctl.flash = flash;
    flash_ctl.flash_size = flash_size;
    flash_ctl.persist = persist;
    flash_ctl.flags = flags;
    flash_ctl.base_s = map->flash_base_s;
    flash_ctl.base_ns = map->flash_base_ns;
    mm_memmap_set_flash_writer(map, flash_write_cb, &flash_ctl);
}
