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

/* MCXN947 S50 ELS: the register sequence wolfBoot's hal_trng (RND_REQ) and
 * DICE HAL (KDELETE, HKDF, KEYGEN, ECSIGN through mcuxClEls) drive. */

#include <stdio.h>
#include <string.h>
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/cpu_db.h"
#include "mcxn947/mcxn947_mmio.h"
#include "mcxn947/mcxn947_els.h"

#ifdef M33MU_HAS_WOLFSSL
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/ecc.h>
#endif

#define ELS                 0x50054000u
#define ELS_NS              0x40054000u
#define ELS_STATUS          0x000u
#define ELS_CTRL            0x004u
#define ELS_CMDCFG0         0x008u
#define ELS_KIDX0           0x010u
#define ELS_KIDX1           0x014u
#define ELS_KPROPIN         0x018u
#define ELS_DMA_SRC0        0x020u
#define ELS_DMA_RES0        0x038u
#define ELS_DMA_RES0_LEN    0x03Cu
#define ELS_INT_STATUS_CLR  0x044u
#define ELS_ERR_STATUS      0x04Cu
#define ELS_ERR_STATUS_CLR  0x050u
#define ELS_DMA_FIN_ADDR    0x070u
#define ELS_KS0             0x150u

#define STATUS_BUSY         (1u << 0)
#define STATUS_IRQ          (1u << 1)
#define STATUS_ERR          (1u << 2)
#define STATUS_PRNG_RDY     (1u << 3)
#define CTRL_EN             (1u << 0)
#define CTRL_START          (1u << 1)
#define CTRL_CMD(c)         ((mm_u32)(c) << 3)
#define CTRL_BYTE_ORDER_BE  (1u << 8)
#define ERR_BUS             (1u << 0)
#define ERR_OPN             (1u << 1)
#define KS_KACT             (1u << 5)

#define CMD_ECSIGN          4u
#define CMD_KEYGEN          8u
#define CMD_KDELETE         11u
#define CMD_HKDF            17u
#define CMD_RND_REQ         24u

#define CDI_INITIAL_SLOT    7u
#define CDI_DERIVED_SLOT    11u
#define KPROP_CDI           0x84020060u
#define KPROP_IAK           0x80044060u

#define RAM_S               0x30000000u
#define SYSCON              0x50000000u
#define SYSCON_AHBCLKCTRL2  0x208u
#define SYSCON_CTRL2_ELS    (1u << 18)

static mm_u8 flash[0x200000];
static mm_u8 ram[0x80000];
static struct mm_memmap map;
static struct mmio_region regions[128];

static int fail(const char *what)
{
    printf("mcxn947_els_test: FAIL (%s)\n", what);
    return 1;
}

static mm_bool reg_write(mm_u32 base, mm_u32 off, mm_u32 v)
{
    return mmio_bus_write(&map.mmio, base + off, 4u, v);
}

static mm_u32 reg_read(mm_u32 base, mm_u32 off)
{
    mm_u32 v = 0xDEADBEEFu;
    (void)mmio_bus_read(&map.mmio, base + off, 4u, &v);
    return v;
}

static mm_bool els_start(mm_u32 cmd, mm_u32 cfg, mm_u32 byte_order)
{
    if (!reg_write(ELS, ELS_CMDCFG0, cfg)) return MM_FALSE;
    return reg_write(ELS, ELS_CTRL, CTRL_EN | CTRL_START | CTRL_CMD(cmd) | byte_order);
}

/* mcuxClEls_WaitForOperation(MCUXCLELS_ERROR_FLAGS_CLEAR) */
static mm_u32 els_wait(void)
{
    mm_u32 err;
    while ((reg_read(ELS, ELS_STATUS) & STATUS_BUSY) != 0u) {
    }
    err = ((reg_read(ELS, ELS_STATUS) & STATUS_ERR) != 0u) ? reg_read(ELS, ELS_ERR_STATUS) : 0u;
    (void)reg_write(ELS, ELS_ERR_STATUS_CLR, 1u);
    return err;
}

static void ram_read(mm_u32 addr, mm_u8 *out, mm_u32 len)
{
    mm_u32 i;
    for (i = 0u; i < len; ++i) {
        out[i] = ram[(addr - RAM_S) + i];
    }
}

static void ram_write(mm_u32 addr, const mm_u8 *in, mm_u32 len)
{
    mm_u32 i;
    for (i = 0u; i < len; ++i) {
        ram[(addr - RAM_S) + i] = in[i];
    }
}

static int test_rnd_req(void)
{
    mm_u8 a[32];
    mm_u8 b[32];
    mm_u8 zero[32];

    memset(zero, 0, sizeof(zero));
    /* hal_trng_init: enable and wait for BUSY to clear */
    if (!reg_write(ELS, ELS_CTRL, CTRL_EN)) return fail("ELS_CTRL enable");
    if ((reg_read(ELS, ELS_STATUS) & STATUS_BUSY) != 0u) return fail("busy after enable");
    if ((reg_read(ELS, ELS_STATUS) & STATUS_PRNG_RDY) == 0u) return fail("PRNG not ready");

    if (!reg_write(ELS, ELS_DMA_RES0, RAM_S + 0x100u)) return 1;
    if (!reg_write(ELS, ELS_DMA_RES0_LEN, 32u)) return 1;
    if (!els_start(CMD_RND_REQ, 0u, 0u)) return 1;
    if ((reg_read(ELS, ELS_STATUS) & STATUS_ERR) != 0u) {
        printf("mcxn947_els_test: RND_REQ ERR_STATUS=0x%08lx\n", (unsigned long)reg_read(ELS, ELS_ERR_STATUS));
        return fail("RND_REQ error");
    }
    if (els_wait() != 0u) return fail("RND_REQ error");
    if ((reg_read(ELS, ELS_STATUS) & STATUS_IRQ) == 0u) return fail("IRQ after RND_REQ");
    if (!reg_write(ELS, ELS_INT_STATUS_CLR, 1u)) return 1;
    if ((reg_read(ELS, ELS_STATUS) & STATUS_IRQ) != 0u) return fail("IRQ clear");
    if (reg_read(ELS, ELS_DMA_FIN_ADDR) != RAM_S + 0x120u) return fail("DMA_FIN_ADDR");
    ram_read(RAM_S + 0x100u, a, sizeof(a));
    if (memcmp(a, zero, sizeof(a)) == 0) return fail("RND_REQ wrote nothing");
    if (ram[0x120] != 0u) return fail("RND_REQ overran length");

    if (!els_start(CMD_RND_REQ, 0u, 0u)) return 1;
    if (els_wait() != 0u) return fail("second RND_REQ error");
    ram_read(RAM_S + 0x100u, b, sizeof(b));
    if (memcmp(a, b, sizeof(a)) == 0) return fail("RND_REQ repeated output");

    /* the HAL requests a word for unaligned heads and tails */
    if (!reg_write(ELS, ELS_DMA_RES0_LEN, 4u)) return 1;
    if (!els_start(CMD_RND_REQ, 0u, 0u)) return 1;
    if (els_wait() != 0u) return fail("4-byte RND_REQ error");

    /* lengths that are not a multiple of 4 are a DMA error, and the error
     * sticks until ERR_STATUS_CLR */
    if (!reg_write(ELS, ELS_DMA_RES0_LEN, 6u)) return 1;
    if (!els_start(CMD_RND_REQ, 0u, 0u)) return 1;
    if ((reg_read(ELS, ELS_STATUS) & STATUS_ERR) == 0u) return fail("no ERR for odd length");
    if ((reg_read(ELS, ELS_ERR_STATUS) & ERR_BUS) == 0u) return fail("no BUS_ERR for odd length");
    if (!reg_write(ELS, ELS_ERR_STATUS_CLR, 1u)) return 1;
    if ((reg_read(ELS, ELS_STATUS) & STATUS_ERR) != 0u) return fail("ERR not cleared");

    /* a DMA target outside SRAM is a bus error */
    if (!reg_write(ELS, ELS_DMA_RES0, 0x00001000u)) return 1;
    if (!reg_write(ELS, ELS_DMA_RES0_LEN, 16u)) return 1;
    if (!els_start(CMD_RND_REQ, 0u, 0u)) return 1;
    if ((els_wait() & ERR_BUS) == 0u) return fail("no BUS_ERR for flash target");

    /* a command without ELS_EN is an operational error */
    if (!reg_write(ELS, ELS_DMA_RES0, RAM_S + 0x100u)) return 1;
    if (!reg_write(ELS, ELS_CTRL, CTRL_START | CTRL_CMD(CMD_RND_REQ))) return 1;
    if ((els_wait() & ERR_OPN) == 0u) return fail("no OPN_ERR when disabled");
    if (!reg_write(ELS, ELS_CTRL, CTRL_EN)) return 1;

    /* the Non-secure alias is the same block */
    if (reg_read(ELS_NS, ELS_DMA_RES0) != RAM_S + 0x100u) return fail("NS alias");

    /* gating the ELS clock (SYSCON AHBCLKCTRL2 bit 18) refuses commands */
    if (!reg_write(SYSCON, SYSCON_AHBCLKCTRL2, reg_read(SYSCON, SYSCON_AHBCLKCTRL2) & ~SYSCON_CTRL2_ELS)) return 1;
    if (!reg_write(ELS, ELS_DMA_RES0_LEN, 16u)) return 1;
    if (!els_start(CMD_RND_REQ, 0u, 0u)) return 1;
    if ((els_wait() & ERR_OPN) == 0u) return fail("no OPN_ERR with clock gated");
    if (!reg_write(SYSCON, SYSCON_AHBCLKCTRL2, reg_read(SYSCON, SYSCON_AHBCLKCTRL2) | SYSCON_CTRL2_ELS)) return 1;
    if (!els_start(CMD_RND_REQ, 0u, 0u)) return 1;
    if (els_wait() != 0u) return fail("RND_REQ after clock restored");
    return 0;
}

#ifdef M33MU_HAS_WOLFSSL
static int verify_signature(const mm_u8 pub[64], mm_u32 sig_addr, const mm_u8 hash[32])
{
    mm_u8 sig[64];
    mm_u8 der[80];
    word32 der_len = (word32)sizeof(der);
    ecc_key key;
    int verified = 0;
    int rc;

    ram_read(sig_addr, sig, sizeof(sig));
    if (wc_ecc_init(&key) != 0) return 1;
    rc = wc_ecc_import_unsigned(&key, pub, pub + 32u, 0, ECC_SECP256R1);
    if (rc == 0) rc = wc_ecc_rs_raw_to_sig(sig, 32u, sig + 32u, 32u, der, &der_len);
    if (rc == 0) rc = wc_ecc_verify_hash(der, der_len, hash, 32u, &verified, &key);
    wc_ecc_free(&key);
    return (rc == 0 && verified == 1) ? 0 : 1;
}
#endif

static int test_keystore(void)
{
    mm_u8 deriv[32];
    mm_u8 pub_a[64];
    mm_u8 pub_b[64];
    mm_u8 zero[64];
    mm_u32 i;

    memset(zero, 0, sizeof(zero));
    for (i = 0u; i < sizeof(deriv); ++i) deriv[i] = (mm_u8)(0xA0u + i);
    ram_write(RAM_S + 0x200u, deriv, sizeof(deriv));

    /* ROM DICE leaves the initial CDI in slot 7 */
    if ((reg_read(ELS, ELS_KS0 + CDI_INITIAL_SLOT * 4u) & KS_KACT) == 0u) return fail("ROM CDI slot empty");
    if (!mm_mcxn947_els_slot_active(CDI_INITIAL_SLOT)) return fail("slot query");

    /* hal_dice_update_cdi: KDELETE on an empty slot succeeds, then HKDF */
    if (!reg_write(ELS, ELS_KIDX0, CDI_DERIVED_SLOT)) return 1;
    if (!els_start(CMD_KDELETE, 0u, CTRL_BYTE_ORDER_BE)) return 1;
    if (els_wait() != 0u) return fail("KDELETE empty slot");

    if (!reg_write(ELS, ELS_KIDX0, CDI_INITIAL_SLOT)) return 1;
    if (!reg_write(ELS, ELS_KIDX1, CDI_DERIVED_SLOT)) return 1;
    if (!reg_write(ELS, ELS_KPROPIN, KPROP_CDI)) return 1;
    if (!reg_write(ELS, ELS_DMA_SRC0, RAM_S + 0x200u)) return 1;
    if (!els_start(CMD_HKDF, 0u, CTRL_BYTE_ORDER_BE)) return 1;
    if (els_wait() != 0u) return fail("HKDF");
    if (reg_read(ELS, ELS_KS0 + CDI_DERIVED_SLOT * 4u) != (KPROP_CDI | KS_KACT)) return fail("HKDF slot props");

    /* HKDF from an empty slot is refused */
    if (!reg_write(ELS, ELS_KIDX0, 12u)) return 1;
    if (!reg_write(ELS, ELS_KIDX1, 13u)) return 1;
    if (!els_start(CMD_HKDF, 0u, CTRL_BYTE_ORDER_BE)) return 1;
    if ((els_wait() & ERR_OPN) == 0u) return fail("HKDF empty source");
    if ((reg_read(ELS, ELS_KS0 + 13u * 4u) & KS_KACT) != 0u) return fail("HKDF wrote on error");

#ifdef M33MU_HAS_WOLFSSL
    /* hal_dice_create_attest_key: deterministic KEYGEN from the derived CDI */
    if (!reg_write(ELS, ELS_KIDX0, CDI_DERIVED_SLOT)) return 1;
    if (!reg_write(ELS, ELS_KIDX1, CDI_DERIVED_SLOT)) return 1;
    if (!reg_write(ELS, ELS_KPROPIN, KPROP_IAK)) return 1;
    if (!reg_write(ELS, ELS_DMA_RES0, RAM_S + 0x300u)) return 1;
    if (!els_start(CMD_KEYGEN, 0x12u, CTRL_BYTE_ORDER_BE)) return 1;
    if (els_wait() != 0u) return fail("KEYGEN");
    ram_read(RAM_S + 0x300u, pub_a, sizeof(pub_a));
    if (memcmp(pub_a, zero, sizeof(pub_a)) == 0) return fail("KEYGEN wrote no public key");
    if (reg_read(ELS, ELS_KS0 + CDI_DERIVED_SLOT * 4u) != (KPROP_IAK | KS_KACT)) return fail("KEYGEN slot props");

    /* hal_dice_sign_hash: ECSIGN over a 32-byte hash, verified with wolfSSL */
    ram_write(RAM_S + 0x400u, deriv, sizeof(deriv));
    if (!reg_write(ELS, ELS_KIDX0, CDI_DERIVED_SLOT)) return 1;
    if (!reg_write(ELS, ELS_DMA_SRC0, RAM_S + 0x400u)) return 1;
    if (!reg_write(ELS, ELS_DMA_RES0, RAM_S + 0x500u)) return 1;
    if (!els_start(CMD_ECSIGN, 0x10u, CTRL_BYTE_ORDER_BE)) return 1;
    if (els_wait() != 0u) return fail("ECSIGN");
    if (verify_signature(pub_a, RAM_S + 0x500u, deriv) != 0) return fail("signature does not verify");

    /* the same CDI always yields the same IAK (deterministic keygen) */
    if (!reg_write(ELS, ELS_KIDX0, CDI_DERIVED_SLOT)) return 1;
    if (!els_start(CMD_KDELETE, 0u, CTRL_BYTE_ORDER_BE)) return 1;
    if (els_wait() != 0u) return fail("KDELETE IAK");
    if ((reg_read(ELS, ELS_KS0 + CDI_DERIVED_SLOT * 4u) & KS_KACT) != 0u) return fail("KDELETE left slot active");
    if (!reg_write(ELS, ELS_KIDX0, CDI_INITIAL_SLOT)) return 1;
    if (!reg_write(ELS, ELS_KIDX1, CDI_DERIVED_SLOT)) return 1;
    if (!reg_write(ELS, ELS_KPROPIN, KPROP_CDI)) return 1;
    if (!reg_write(ELS, ELS_DMA_SRC0, RAM_S + 0x200u)) return 1;
    if (!els_start(CMD_HKDF, 0u, CTRL_BYTE_ORDER_BE)) return 1;
    if (els_wait() != 0u) return fail("HKDF again");
    if (!reg_write(ELS, ELS_KIDX0, CDI_DERIVED_SLOT)) return 1;
    if (!reg_write(ELS, ELS_KIDX1, CDI_DERIVED_SLOT)) return 1;
    if (!reg_write(ELS, ELS_KPROPIN, KPROP_IAK)) return 1;
    if (!reg_write(ELS, ELS_DMA_RES0, RAM_S + 0x600u)) return 1;
    if (!els_start(CMD_KEYGEN, 0x12u, CTRL_BYTE_ORDER_BE)) return 1;
    if (els_wait() != 0u) return fail("KEYGEN again");
    ram_read(RAM_S + 0x600u, pub_b, sizeof(pub_b));
    if (memcmp(pub_a, pub_b, sizeof(pub_a)) != 0) return fail("KEYGEN not deterministic");

    /* ECSIGN with an empty slot is refused */
    if (!reg_write(ELS, ELS_KIDX0, 14u)) return 1;
    if (!els_start(CMD_ECSIGN, 0x10u, CTRL_BYTE_ORDER_BE)) return 1;
    if ((els_wait() & ERR_OPN) == 0u) return fail("ECSIGN empty slot");
#else
    (void)pub_a;
    (void)pub_b;
#endif
    return 0;
}

int main(void)
{
    struct mm_target_cfg cfg;

    if (!mm_cpu_lookup("mcxn947", &cfg)) return fail("lookup");
    mm_memmap_init(&map, regions, 128u);
    if (!mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE)) return 1;
    if (!mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE)) return 1;
    mm_mcxn947_mmio_reset();
    if (!mm_mcxn947_register_mmio(&map.mmio)) return fail("register");
    mm_mcxn947_flash_bind(&map, flash, sizeof(flash), 0, 0u);
    mmio_set_active_sec(MM_SECURE);

    if (test_rnd_req() != 0) return 1;
    if (test_keystore() != 0) return 1;

    printf("mcxn947_els_test: PASS\n");
    return 0;
}
