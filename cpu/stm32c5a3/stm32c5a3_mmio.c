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

/* STM32C5A3 system peripherals: RCC, PWR, the embedded flash controller,
 * ICACHE and GPIO.  The part has no TrustZone, so every block has a single
 * view.  The flash controller is the STM32H5 one: two 512 KB banks of 8 KB
 * pages, a 128-bit write buffer, and the SWAP_BANK option that remaps the
 * banks at the next reset. */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "stm32c5a3/stm32c5a3_mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/gpio.h"
#include "m33mu/nvic.h"
#include "stm32_gpio.h"

#define RCC_BASE       0x44020C00u
#define RCC_SIZE       0x400u
#define RCC_CR1        0x000u
#define RCC_CR2        0x004u
#define RCC_CFGR1      0x01Cu
#define RCC_CFGR2      0x020u
#define RCC_AHB2ENR    0x08Cu
#define RCC_RTCCR      0x0F0u

#define RCC_CR1_HSISON     (1u << 0)
#define RCC_CR1_HSIDIV3ON  (1u << 1)
#define RCC_CR1_HSIKON     (1u << 2)
#define RCC_CR1_HSISRDY    (1u << 4)
#define RCC_CR1_HSIDIV3RDY (1u << 5)
#define RCC_CR1_HSIKRDY    (1u << 6)
#define RCC_CR1_PSISON     (1u << 8)
#define RCC_CR1_PSIDIV3ON  (1u << 9)
#define RCC_CR1_PSIKON     (1u << 10)
#define RCC_CR1_PSISRDY    (1u << 12)
#define RCC_CR1_PSIDIV3RDY (1u << 13)
#define RCC_CR1_PSIKRDY    (1u << 14)
#define RCC_CR1_HSEON      (1u << 16)
#define RCC_CR1_HSERDY     (1u << 17)
#define RCC_RTCCR_LSEON    (1u << 0)
#define RCC_RTCCR_LSERDY   (1u << 1)

#define PWR_BASE       0x44020800u
#define PWR_SIZE       0x400u

#define FLASH_BASE     0x40022000u
#define FLASH_SIZE     0x400u
#define FLASH_ACR      0x000u
#define FLASH_KEYR     0x004u
#define FLASH_OPTKEYR  0x00Cu
#define FLASH_OPSR     0x018u
#define FLASH_OPTCR    0x01Cu
#define FLASH_SR       0x020u
#define FLASH_CR       0x028u
#define FLASH_CCR      0x030u
#define FLASH_OPTSR_CUR 0x050u
#define FLASH_OPTSR_PRG 0x054u
#define FLASH_ECCDETR  0x104u

#define FLASH_KEY1     0x45670123u
#define FLASH_KEY2     0xCDEF89ABu
#define FLASH_OPTKEY1  0x08192A3Bu
#define FLASH_OPTKEY2  0x4C5D6E7Fu

#define FLASH_SR_BSY     (1u << 0)
#define FLASH_SR_WBNE    (1u << 1)
#define FLASH_SR_EOP     (1u << 16)
#define FLASH_SR_WRPERR  (1u << 17)
#define FLASH_SR_PGSERR  (1u << 18)
#define FLASH_SR_STRBERR (1u << 19)
#define FLASH_SR_INCERR  (1u << 20)
#define FLASH_SR_OPTCHANGEERR (1u << 23)
#define FLASH_CCR_MASK   (FLASH_SR_EOP | FLASH_SR_WRPERR | FLASH_SR_PGSERR | \
                          FLASH_SR_STRBERR | FLASH_SR_INCERR | FLASH_SR_OPTCHANGEERR)

#define FLASH_CR_LOCK    (1u << 0)
#define FLASH_CR_PG      (1u << 1)
#define FLASH_CR_PER     (1u << 2)
#define FLASH_CR_BER     (1u << 3)
#define FLASH_CR_FW      (1u << 4)
#define FLASH_CR_STRT    (1u << 5)
#define FLASH_CR_PNB_SHIFT 6
#define FLASH_CR_PNB_MASK (0x3Fu << FLASH_CR_PNB_SHIFT)
#define FLASH_CR_MER     (1u << 15)
#define FLASH_CR_BKSEL   (1u << 31)

#define FLASH_OPTCR_OPTLOCK   (1u << 0)
#define FLASH_OPTCR_OPTSTRT   (1u << 1)
#define FLASH_OPTCR_SWAP_BANK (1u << 31)
#define FLASH_OPTSR_SWAP_BANK (1u << 31)
#define FLASH_OPTSR_RDP_LEVEL0 (0xAAu << 8)

#define FLASH_PAGE_SIZE  0x2000u
#define FLASH_BANK_COUNT 2u
#define FLASH_WORD_SIZE  16u

#define ICACHE_BASE     0x40030400u
#define ICACHE_SIZE     0x400u
#define ICACHE_CR       0x000u
#define ICACHE_SR       0x004u
#define ICACHE_FCR      0x00Cu
#define ICACHE_CR_CACHEINV (1u << 1)
#define ICACHE_SR_BSYENDF  (1u << 1)
#define ICACHE_SR_ERRF     (1u << 2)
#define ICACHE_CR_RESET    0x00000004u

#define GPIO_BASE       0x42020000u
#define GPIO_BANK_COUNT 8  /* A..H */

struct rcc_state {
    mm_u32 regs[RCC_SIZE / 4];
    mm_u64 cpu_hz;
};

struct simple_blk {
    mm_u32 regs[PWR_SIZE / 4];
};

struct icache_state {
    mm_u32 regs[ICACHE_SIZE / 4];
};

struct flash_state {
    mm_u32 regs[FLASH_SIZE / 4];
    mm_u8 *flash;
    mm_u32 flash_size;
    mm_u32 base;
    const struct mm_flash_persist *persist;
    mm_u32 flags;
    mm_u8 key_stage;
    mm_u8 opt_key_stage;
    mm_bool dualbank_enabled;
    mm_bool swap_active;            /* the banks as they are mapped now (FLASH_OPTCR) */
    /* the flash word being assembled from consecutive stores (SR.WBNE) */
    mm_bool wb_valid;
    mm_u32 wb_off;
    mm_u32 wb_mask;
    mm_u8 wb_data[FLASH_WORD_SIZE];
};

static struct rcc_state rcc;
static struct simple_blk pwr;
static struct icache_state icache;
static struct flash_state flash_ctl;
static struct stm32_gpio_state gpio[GPIO_BANK_COUNT];
static struct stm32_gpio_ctx gpio_ctx_data[GPIO_BANK_COUNT];
/* SWAP_BANK option byte: programmed by an option change, applied at reset */
static mm_bool g_opt_swap = MM_FALSE;

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
    mm_u32 cr1 = r->regs[RCC_CR1 / 4];
    mm_u32 rtccr = r->regs[RCC_RTCCR / 4];
    /* oscillators and the PSI are ready as soon as they are on */
    if ((cr1 & RCC_CR1_HSISON) != 0u) cr1 |= RCC_CR1_HSISRDY; else cr1 &= ~RCC_CR1_HSISRDY;
    if ((cr1 & RCC_CR1_HSIDIV3ON) != 0u) cr1 |= RCC_CR1_HSIDIV3RDY; else cr1 &= ~RCC_CR1_HSIDIV3RDY;
    if ((cr1 & RCC_CR1_HSIKON) != 0u) cr1 |= RCC_CR1_HSIKRDY; else cr1 &= ~RCC_CR1_HSIKRDY;
    if ((cr1 & RCC_CR1_PSISON) != 0u) cr1 |= RCC_CR1_PSISRDY; else cr1 &= ~RCC_CR1_PSISRDY;
    if ((cr1 & RCC_CR1_PSIDIV3ON) != 0u) cr1 |= RCC_CR1_PSIDIV3RDY; else cr1 &= ~RCC_CR1_PSIDIV3RDY;
    if ((cr1 & RCC_CR1_PSIKON) != 0u) cr1 |= RCC_CR1_PSIKRDY; else cr1 &= ~RCC_CR1_PSIKRDY;
    if ((cr1 & RCC_CR1_HSEON) != 0u) cr1 |= RCC_CR1_HSERDY; else cr1 &= ~RCC_CR1_HSERDY;
    r->regs[RCC_CR1 / 4] = cr1;
    if ((rtccr & RCC_RTCCR_LSEON) != 0u) rtccr |= RCC_RTCCR_LSERDY; else rtccr &= ~RCC_RTCCR_LSERDY;
    r->regs[RCC_RTCCR / 4] = rtccr;
}

/* SYSCLK: HSIDIV3 (48 MHz), HSIS (144 MHz), HSE (48 MHz on NUCLEO-C5A3ZG)
 * or the PSIS.  Only the 144 MHz PSI output (PSIFREQ=1) is known. */
static void rcc_update_sysclk(struct rcc_state *r)
{
    mm_u32 cfgr1 = r->regs[RCC_CFGR1 / 4];
    mm_u32 hpre = r->regs[RCC_CFGR2 / 4] & 0xFu;
    mm_u32 sw = cfgr1 & 0x3u;
    mm_u64 sys = 0;
    mm_u32 div = 1u;

    if (sw == 0u) sys = 48000000ull;
    else if (sw == 1u) sys = 144000000ull;
    else if (sw == 2u) sys = 48000000ull;
    else sys = 144000000ull;
    if (hpre >= 8u) {
        div = (hpre <= 11u) ? (2u << (hpre - 8u)) : (64u << (hpre - 12u));
    }
    r->cpu_hz = sys / (mm_u64)div;
    if (r->cpu_hz == 0) r->cpu_hz = 1;
    r->regs[RCC_CFGR1 / 4] = (cfgr1 & ~(0x3u << 3)) | (sw << 3);
}

static mm_bool rcc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rcc_state *r = (struct rcc_state *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RCC_SIZE) return MM_FALSE;
    if (offset <= RCC_CR1 && (offset + size_bytes) > RCC_CR1) {
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
    if (offset == RCC_CR1 || offset == RCC_RTCCR) {
        rcc_update_ready(r);
    }
    if (offset == RCC_CR1 || offset == RCC_CR2 || offset == RCC_CFGR1 || offset == RCC_CFGR2) {
        rcc_update_sysclk(r);
    }
    return MM_TRUE;
}

/* ---- PWR (register file) ---- */

static mm_bool simple_blk_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct simple_blk *b = (struct simple_blk *)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > PWR_SIZE) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)b->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool simple_blk_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct simple_blk *b = (struct simple_blk *)opaque;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > PWR_SIZE) return MM_FALSE;
    memcpy((mm_u8 *)b->regs + offset, &value, size_bytes);
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

static void flash_apply_bank_swap(struct flash_state *f)
{
    mm_u32 bank_size;
    mm_u32 i;
    if (f->flash == 0 || f->flash_size == 0u || (f->flash_size % 2u) != 0u) {
        return;
    }
    bank_size = f->flash_size / 2u;
    for (i = 0; i < bank_size; ++i) {
        mm_u8 tmp = f->flash[i];
        f->flash[i] = f->flash[i + bank_size];
        f->flash[i + bank_size] = tmp;
    }
    if (f->persist != 0 && f->persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)f->persist, 0u, f->flash_size);
    }
}

static void flash_set_swap_state(struct flash_state *f, mm_bool swap_active)
{
    if (!f->dualbank_enabled) {
        if (f->swap_active) {
            flash_apply_bank_swap(f);
        }
        f->swap_active = MM_FALSE;
        return;
    }
    if (f->swap_active == swap_active) {
        return;
    }
    f->swap_active = swap_active;
    flash_apply_bank_swap(f);
}

/* FLASH_OPTSR_CUR and _PRG show the SWAP_BANK option bit; FLASH_OPTCR shows
 * whether the banks are swapped now.  They differ between an option change
 * and the reset that applies it. */
static void flash_sync_option_regs(struct flash_state *f)
{
    mm_u32 swap_bit;
    if (!f->dualbank_enabled) {
        f->swap_active = MM_FALSE;
        g_opt_swap = MM_FALSE;
    }
    swap_bit = g_opt_swap ? FLASH_OPTSR_SWAP_BANK : 0u;
    f->regs[FLASH_OPTSR_CUR / 4u] = (f->regs[FLASH_OPTSR_CUR / 4u] & ~FLASH_OPTSR_SWAP_BANK) | swap_bit;
    f->regs[FLASH_OPTSR_PRG / 4u] = (f->regs[FLASH_OPTSR_PRG / 4u] & ~FLASH_OPTSR_SWAP_BANK) | swap_bit;
    f->regs[FLASH_OPTCR / 4u] = (f->regs[FLASH_OPTCR / 4u] & ~FLASH_OPTCR_SWAP_BANK) |
                                (f->swap_active ? FLASH_OPTCR_SWAP_BANK : 0u);
}

static void flash_set_eop(void)
{
    flash_ctl.regs[FLASH_SR / 4u] |= FLASH_SR_EOP;
}

static mm_bool flash_is_unlocked(void)
{
    return (flash_ctl.regs[FLASH_CR / 4u] & FLASH_CR_LOCK) == 0u;
}

static mm_bool flash_opt_unlocked(void)
{
    return (flash_ctl.regs[FLASH_OPTCR / 4u] & FLASH_OPTCR_OPTLOCK) == 0u;
}

static void flash_handle_key(mm_u32 value)
{
    if (flash_ctl.key_stage == 0u) {
        flash_ctl.key_stage = (value == FLASH_KEY1) ? 1u : 0u;
        return;
    }
    if (value == FLASH_KEY2) {
        flash_ctl.regs[FLASH_CR / 4u] &= ~FLASH_CR_LOCK;
    }
    flash_ctl.key_stage = 0u;
}

/* a wrong key keeps FLASH_OPTCR locked until the next reset */
static void flash_handle_opt_key(mm_u32 value)
{
    if (flash_ctl.opt_key_stage == 0u && value == FLASH_OPTKEY1) {
        flash_ctl.opt_key_stage = 1u;
    } else if (flash_ctl.opt_key_stage == 1u && value == FLASH_OPTKEY2) {
        flash_ctl.regs[FLASH_OPTCR / 4u] &= ~FLASH_OPTCR_OPTLOCK;
        flash_ctl.opt_key_stage = 0u;
    } else {
        flash_ctl.opt_key_stage = 2u;
    }
}

static void flash_wb_drop(void)
{
    flash_ctl.wb_valid = MM_FALSE;
    flash_ctl.wb_mask = 0u;
    flash_ctl.regs[FLASH_SR / 4u] &= ~FLASH_SR_WBNE;
}

/* Program the flash word the write buffer holds: one operation, all 128
 * bits, the bytes never written left at 1.  With `force` (FW) a buffer that
 * clears no bit programs nothing.  A flash word is never programmed twice:
 * over one that is not erased the program is refused with PGSERR. */
static void flash_wb_commit(mm_bool force)
{
    mm_u8 data[FLASH_WORD_SIZE];
    mm_u32 off = flash_ctl.wb_off;
    mm_bool clears = MM_FALSE;
    mm_u32 i;

    if (!flash_ctl.wb_valid) {
        return;
    }
    memcpy(data, flash_ctl.wb_data, sizeof(data));
    flash_wb_drop();
    for (i = 0; i < FLASH_WORD_SIZE; ++i) {
        if (data[i] != 0xFFu) {
            clears = MM_TRUE;
        }
    }
    if (force && !clears) {
        return;
    }
    if ((flash_ctl.flags & MM_TARGET_FLAG_NVM_WRITEONCE) != 0u) {
        for (i = 0; i < FLASH_WORD_SIZE; ++i) {
            if (flash_ctl.flash[off + i] != 0xFFu) {
                flash_ctl.regs[FLASH_SR / 4u] |= FLASH_SR_PGSERR;
                return;
            }
        }
    }
    for (i = 0; i < FLASH_WORD_SIZE; ++i) {
        flash_ctl.flash[off + i] &= data[i];
    }
    flash_set_eop();
    if (flash_ctl.persist != 0 && flash_ctl.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)flash_ctl.persist, off, FLASH_WORD_SIZE);
    }
}

/* A store with PG set goes into the write buffer.  A store to another flash
 * word before the buffer is full is an inconsistency: INCERR, both lost. */
static mm_bool flash_wb_store(mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 word = offset & ~(FLASH_WORD_SIZE - 1u);
    mm_u32 at = offset - word;
    mm_u32 i;

    if ((size_bytes != 1u && size_bytes != 2u && size_bytes != 4u) || at + size_bytes > FLASH_WORD_SIZE) {
        return MM_FALSE;
    }
    if (flash_ctl.wb_valid && flash_ctl.wb_off != word) {
        flash_ctl.regs[FLASH_SR / 4u] |= FLASH_SR_INCERR;
        flash_wb_drop();
        return MM_TRUE;
    }
    if (!flash_ctl.wb_valid) {
        memset(flash_ctl.wb_data, 0xFF, sizeof(flash_ctl.wb_data));
        flash_ctl.wb_valid = MM_TRUE;
        flash_ctl.wb_off = word;
        flash_ctl.wb_mask = 0u;
    }
    for (i = 0; i < size_bytes; ++i) {
        flash_ctl.wb_data[at + i] = (mm_u8)((value >> (8u * i)) & 0xFFu);
        flash_ctl.wb_mask |= 1u << (at + i);
    }
    flash_ctl.regs[FLASH_SR / 4u] |= FLASH_SR_WBNE;
    if (flash_ctl.wb_mask == 0xFFFFu) {
        flash_wb_commit(MM_FALSE);
    }
    return MM_TRUE;
}

/* BKSEL and PNB name physical banks and pages whatever the SWAP_BANK
 * setting; the backing array holds the logical (post-swap) view, so the
 * bank selection is inverted while the banks are swapped. */
static void flash_apply_erase(void)
{
    mm_u32 cr = flash_ctl.regs[FLASH_CR / 4u];
    mm_u32 bank_size;
    mm_u32 start;
    mm_u32 length;
    mm_bool bank2;
    if (flash_ctl.flash == 0 || flash_ctl.flash_size == 0) {
        return;
    }
    bank_size = flash_ctl.flash_size / FLASH_BANK_COUNT;
    bank2 = ((cr & FLASH_CR_BKSEL) != 0u) ? MM_TRUE : MM_FALSE;
    if (flash_ctl.swap_active) {
        bank2 = bank2 ? MM_FALSE : MM_TRUE;
    }
    if ((cr & FLASH_CR_MER) != 0u) {
        start = 0u;
        length = flash_ctl.flash_size;
    } else if ((cr & FLASH_CR_BER) != 0u) {
        start = bank2 ? bank_size : 0u;
        length = bank_size;
    } else if ((cr & FLASH_CR_PER) != 0u) {
        start = ((cr & FLASH_CR_PNB_MASK) >> FLASH_CR_PNB_SHIFT) * FLASH_PAGE_SIZE;
        if (bank2) {
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
        flash_ctl.regs[FLASH_SR / 4u] |= FLASH_SR_PGSERR;
        return;
    }
    if (flash_trace_enabled()) {
        printf("[FLASH_ERASE] NS mode=%s pnb=%lu start=0x%08lx len=0x%08lx\n",
               (cr & FLASH_CR_MER) ? "MER" : ((cr & FLASH_CR_BER) ? "BER" : "PER"),
               (unsigned long)((cr & FLASH_CR_PNB_MASK) >> FLASH_CR_PNB_SHIFT),
               (unsigned long)start,
               (unsigned long)length);
    }
    memset(flash_ctl.flash + start, 0xFF, length);
    flash_set_eop();
    if (flash_ctl.persist != 0 && flash_ctl.persist->enabled) {
        mm_flash_persist_flush((struct mm_flash_persist *)flash_ctl.persist, start, length);
    }
}

static mm_bool flash_write_cb(void *opaque, enum mm_sec_state sec, mm_u32 addr, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 offset;
    (void)opaque;
    (void)sec;

    if (flash_ctl.flash == 0 || flash_ctl.flash_size == 0) {
        return MM_FALSE;
    }
    if (addr < flash_ctl.base || addr - flash_ctl.base >= flash_ctl.flash_size) {
        return MM_FALSE;
    }
    offset = addr - flash_ctl.base;
    if (offset + size_bytes > flash_ctl.flash_size) {
        return MM_FALSE;
    }
    if (flash_trace_enabled()) {
        printf("[FLASH_WRITE] NS addr=0x%08lx size=%lu value=0x%08lx\n",
               (unsigned long)addr,
               (unsigned long)size_bytes,
               (unsigned long)value);
    }
    if (!flash_is_unlocked() || (flash_ctl.regs[FLASH_CR / 4u] & FLASH_CR_PG) == 0u) {
        /* a store with PG clear is a programming sequence error */
        flash_ctl.regs[FLASH_SR / 4u] |= FLASH_SR_PGSERR;
        return MM_TRUE;
    }
    return flash_wb_store(offset, size_bytes, value);
}

/* An option byte change (OPTSTRT): FLASH_OPTSR_PRG becomes the option bytes
 * in force (_CUR); a new SWAP_BANK waits for the next reset. */
static void flash_option_change(struct flash_state *f)
{
    if (f->wb_valid) {
        f->regs[FLASH_SR / 4u] |= FLASH_SR_PGSERR;
        return;
    }
    g_opt_swap = (f->dualbank_enabled &&
                  (f->regs[FLASH_OPTSR_PRG / 4u] & FLASH_OPTSR_SWAP_BANK) != 0u) ? MM_TRUE : MM_FALSE;
    f->regs[FLASH_OPTSR_CUR / 4u] = f->regs[FLASH_OPTSR_PRG / 4u];
    flash_sync_option_regs(f);
    flash_set_eop();
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
    mm_u32 old;
    if (size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FLASH_SIZE) return MM_FALSE;

    if (offset == FLASH_KEYR) {
        flash_handle_key(value);
        return MM_TRUE;
    }
    if (offset == FLASH_OPTKEYR) {
        flash_handle_opt_key(value);
        return MM_TRUE;
    }
    if (offset == FLASH_SR || offset == FLASH_OPSR || offset == FLASH_OPTSR_CUR) {
        return MM_TRUE;                             /* status: read-only */
    }
    if (offset == FLASH_CCR) {
        f->regs[FLASH_SR / 4u] &= ~(value & FLASH_CCR_MASK);
        return MM_TRUE;
    }
    if (offset == FLASH_OPTCR) {
        old = f->regs[FLASH_OPTCR / 4u];
        /* OPTLOCK is set by software and cleared by the keys; SWAP_BANK is
         * the mapping in force; OPTSTRT is a command */
        f->regs[FLASH_OPTCR / 4u] = (value & ~(FLASH_OPTCR_OPTLOCK | FLASH_OPTCR_SWAP_BANK | FLASH_OPTCR_OPTSTRT)) |
                                    (old & (FLASH_OPTCR_OPTLOCK | FLASH_OPTCR_SWAP_BANK)) |
                                    (value & FLASH_OPTCR_OPTLOCK);
        if ((value & FLASH_OPTCR_OPTSTRT) != 0u) {
            if ((old & FLASH_OPTCR_OPTLOCK) == 0u) {
                flash_option_change(f);
            } else {
                f->regs[FLASH_SR / 4u] |= FLASH_SR_OPTCHANGEERR;
            }
        }
        return MM_TRUE;
    }
    if (offset == FLASH_OPTSR_PRG) {
        if (flash_opt_unlocked()) {
            if (!f->dualbank_enabled) {
                value &= ~FLASH_OPTSR_SWAP_BANK;
            }
            f->regs[FLASH_OPTSR_PRG / 4u] = value;
        }
        return MM_TRUE;
    }
    if (offset == FLASH_ECCDETR) {
        if ((value & (1u << 31)) != 0u) {
            f->regs[offset / 4u] &= ~(1u << 31);    /* ECCD: write 1 to clear */
        }
        return MM_TRUE;
    }
    if (offset == FLASH_CR) {
        if (!flash_is_unlocked()) {
            return MM_TRUE;                         /* locked: every bit is read-only */
        }
        memcpy((mm_u8 *)f->regs + offset, &value, size_bytes);
        f->regs[FLASH_CR / 4u] &= ~(FLASH_CR_STRT | FLASH_CR_FW);
        if ((value & FLASH_CR_FW) != 0u) {
            flash_wb_commit(MM_TRUE);               /* force out a partial flash word */
        }
        if ((value & FLASH_CR_STRT) != 0u) {
            if (f->wb_valid) {
                f->regs[FLASH_SR / 4u] |= FLASH_SR_PGSERR;   /* erase with WBNE set */
            } else {
                flash_apply_erase();
            }
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
    return ((rcc.regs[RCC_AHB2ENR / 4] >> bank) & 1u) != 0u;
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

static mm_bool gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    (void)opaque;
    if (bank < 0 || bank >= GPIO_BANK_COUNT) {
        return MM_FALSE;
    }
    if (name_out != 0 && name_len > 0) {
        snprintf(name_out, name_len, "GPIO%c", 'A' + bank);
    }
    if (pins_out != 0) {
        *pins_out = 16;
    }
    return MM_TRUE;
}

static void gpio_reset_all(void)
{
    size_t i;
    for (i = 0; i < GPIO_BANK_COUNT; ++i) {
        stm32_gpio_reset(&gpio[i], (int)i);
        gpio_ctx_data[i].gpio = &gpio[i];
        gpio_ctx_data[i].is_secure_alias = MM_FALSE;
        gpio_ctx_data[i].bank_index = (int)i;
        gpio_ctx_data[i].clock_enabled = gpio_clock_enabled;
        gpio_ctx_data[i].exti_update = 0;
    }
    mm_gpio_bank_set_reader(gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(gpio_bank_clock, 0);
    mm_gpio_set_bank_info_reader(gpio_bank_info, 0);
}

/* ---- target hooks ---- */

static void soc_reset_regs(void)
{
    /* the banks stay mapped as they are; a SWAP_BANK option change made
     * since the last reset is applied when the flash is bound again */
    mm_bool swap_active = flash_ctl.swap_active;
    mm_bool dualbank = flash_ctl.dualbank_enabled;
    memset(&rcc, 0, sizeof(rcc));
    memset(&pwr, 0, sizeof(pwr));
    memset(&icache, 0, sizeof(icache));
    memset(&flash_ctl, 0, sizeof(flash_ctl));
    flash_ctl.swap_active = swap_active;
    flash_ctl.dualbank_enabled = dualbank;
    gpio_reset_all();
    /* HSIDIV3 drives SYSCLK out of reset (RCC_CR1, RCC_CFGR1) */
    rcc.regs[RCC_CR1 / 4u] = RCC_CR1_HSISON | RCC_CR1_HSIDIV3ON;
    rcc_update_ready(&rcc);
    rcc_update_sysclk(&rcc);
    icache.regs[ICACHE_CR / 4u] = ICACHE_CR_RESET;
    flash_ctl.regs[FLASH_CR / 4u] = FLASH_CR_LOCK;
    flash_ctl.regs[FLASH_OPTCR / 4u] = FLASH_OPTCR_OPTLOCK;
    flash_ctl.regs[FLASH_OPTSR_CUR / 4u] = FLASH_OPTSR_RDP_LEVEL0;
    flash_ctl.regs[FLASH_OPTSR_PRG / 4u] = FLASH_OPTSR_RDP_LEVEL0;
    flash_sync_option_regs(&flash_ctl);
}

void mm_stm32c5a3_mmio_reset(void)
{
    soc_reset_regs();
}

mm_u32 *mm_stm32c5a3_rcc_regs(void)
{
    return rcc.regs;
}

mm_u64 mm_stm32c5a3_cpu_hz(void)
{
    return rcc.cpu_hz;
}

mm_bool mm_stm32c5a3_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    size_t i;

    soc_reset_regs();
    memset(&reg, 0, sizeof(reg));

    reg.base = RCC_BASE;
    reg.size = RCC_SIZE;
    reg.opaque = &rcc;
    reg.read = rcc_read;
    reg.write = rcc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = PWR_BASE;
    reg.size = PWR_SIZE;
    reg.opaque = &pwr;
    reg.read = simple_blk_read;
    reg.write = simple_blk_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = FLASH_BASE;
    reg.size = FLASH_SIZE;
    reg.opaque = &flash_ctl;
    reg.read = flash_read;
    reg.write = flash_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = ICACHE_BASE;
    reg.size = ICACHE_SIZE;
    reg.opaque = &icache;
    reg.read = icache_read;
    reg.write = icache_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = STM32_GPIO_REG_SIZE;
    reg.read = stm32_gpio_read;
    reg.write = stm32_gpio_write;
    for (i = 0; i < GPIO_BANK_COUNT; ++i) {
        reg.base = GPIO_BASE + (mm_u32)i * 0x400u;
        reg.opaque = &gpio_ctx_data[i];
        if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    }
    return MM_TRUE;
}

void mm_stm32c5a3_flash_bind(struct mm_memmap *map,
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
    flash_ctl.base = map->flash_base_ns;
    flash_ctl.dualbank_enabled = (flags & MM_TARGET_FLAG_DUALBANK) != 0u;
    if (!flash_ctl.dualbank_enabled) {
        g_opt_swap = MM_FALSE;
    }
    if (flash_ctl.swap_active != g_opt_swap) {
        /* a SWAP_BANK option change takes effect at reset */
        flash_set_swap_state(&flash_ctl, g_opt_swap);
    }
    flash_sync_option_regs(&flash_ctl);
    mm_memmap_set_flash_writer(map, flash_write_cb, &flash_ctl);
}
